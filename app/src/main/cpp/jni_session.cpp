/*
 * jni_session.cpp Ã¢â‚¬â€ aasdk session lifecycle + JNI callback dispatch.
 *
 * This is the core integration: creates the aasdk pipeline, implements
 * all AA channel event handlers, and fires JNI callbacks to Kotlin.
 *
 * Based on bridge/openautolink/headless/src/live_session.cpp Ã¢â‚¬â€ same
 * aasdk API calls, but output goes to JNI instead of OAL TCP.
 */
#include "jni_session.h"
#include "jni_transport.h"
#include "jni_channel_handlers.h"

#include <android/log.h>

// Protobuf messages for SDR building
#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/PingRequest.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/control/message/DriverPosition.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>
#include <aap_protobuf/service/media/sink/MediaSinkService.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h>
#include <aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h>
#include <aap_protobuf/service/sensorsource/SensorSourceService.pb.h>
#include <aap_protobuf/service/inputsource/InputSourceService.pb.h>
#include <aap_protobuf/service/bluetooth/BluetoothService.pb.h>
#include <aap_protobuf/service/bluetooth/message/BluetoothPairingMethod.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchScreenType.pb.h>
#include <aap_protobuf/service/navigationstatus/NavigationStatusService.pb.h>
#include <aap_protobuf/shared/MessageStatus.pb.h>

#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/KeyEvent.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>

#include <aasdk/Channel/Promise.hpp>
#include <aasdk/Messenger/ChannelId.hpp>
#include <aasdk/Messenger/Message.hpp>
#include <aasdk/Messenger/MessageId.hpp>
#include <aap_protobuf/service/control/ControlMessageType.pb.h>

#define LOG_TAG "OAL-JniSession"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace openautolink::jni {

// ============================================================================
// Lifecycle
// ============================================================================

JniSession::JniSession(JavaVM* jvm)
    : jvm_(jvm)
{
    ioService_ = std::make_unique<boost::asio::io_service>();
    ioWork_ = std::make_unique<boost::asio::io_service::work>(*ioService_);
    strand_ = std::make_unique<boost::asio::io_service::strand>(*ioService_);
    ioThread_ = std::thread(&JniSession::ioServiceThreadFunc, this);
    LOGI("JniSession created");
}

JniSession::~JniSession()
{
    stop();
    strand_.reset();
    LOGI("JniSession destroyed");
}

void JniSession::ioServiceThreadFunc()
{
    LOGI("io_service thread started");
    ioService_->run();
    LOGI("io_service thread exiting");
}

// ============================================================================
// JNI helpers
// ============================================================================

JNIEnv* JniSession::getEnv(bool& attached)
{
    JNIEnv* env = nullptr;
    attached = false;
    jint result = jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        jvm_->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    return env;
}

void JniSession::releaseEnv(bool attached)
{
    if (attached) jvm_->DetachCurrentThread();
}

void JniSession::callVoidCallback(jmethodID method)
{
    if (!callbackRef_ || !method) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) env->CallVoidMethod(callbackRef_, method);
    releaseEnv(attached);
}

// ============================================================================
// start() Ã¢â‚¬â€ build the aasdk pipeline
// ============================================================================

void JniSession::start(JNIEnv* env, jobject transportPipe, jobject callback, jobject sdrConfig)
{
    if (streaming_) {
        LOGW("Session already streaming, ignoring start()");
        return;
    }

    // Create global refs
    callbackRef_ = env->NewGlobalRef(callback);
    jobject transportRef = env->NewGlobalRef(transportPipe);

    // Cache callback method IDs
    jclass cbClass = env->GetObjectClass(callback);
    cbMethods_.onSessionStarted = env->GetMethodID(cbClass, "onSessionStarted", "()V");
    cbMethods_.onSessionStopped = env->GetMethodID(cbClass, "onSessionStopped", "(Ljava/lang/String;)V");
    cbMethods_.onVideoFrame = env->GetMethodID(cbClass, "onVideoFrame", "([BJIIZ)V");
    cbMethods_.onAudioFrame = env->GetMethodID(cbClass, "onAudioFrame", "([BIII)V");
    cbMethods_.onMicRequest = env->GetMethodID(cbClass, "onMicRequest", "(Z)V");
    cbMethods_.onNavigationStatus = env->GetMethodID(cbClass, "onNavigationStatus", "(I)V");
    cbMethods_.onNavigationTurn = env->GetMethodID(cbClass, "onNavigationTurn",
        "(Ljava/lang/String;Ljava/lang/String;[B)V");
    cbMethods_.onNavigationDistance = env->GetMethodID(cbClass, "onNavigationDistance",
        "(IILjava/lang/String;Ljava/lang/String;)V");
    cbMethods_.onMediaMetadata = env->GetMethodID(cbClass, "onMediaMetadata",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V");
    cbMethods_.onMediaPlayback = env->GetMethodID(cbClass, "onMediaPlayback", "(IJ)V");
    cbMethods_.onPhoneStatus = env->GetMethodID(cbClass, "onPhoneStatus", "(II)V");
    cbMethods_.onPhoneBattery = env->GetMethodID(cbClass, "onPhoneBattery", "(IZ)V");
    cbMethods_.onVoiceSession = env->GetMethodID(cbClass, "onVoiceSession", "(Z)V");
    cbMethods_.onAudioFocusRequest = env->GetMethodID(cbClass, "onAudioFocusRequest", "(I)V");
    cbMethods_.onError = env->GetMethodID(cbClass, "onError", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cbClass);

    // Read SDR config from Kotlin
    jclass sdrClass = env->GetObjectClass(sdrConfig);
    sdrConfig_.videoWidth = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoWidth", "I"));
    sdrConfig_.videoHeight = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoHeight", "I"));
    sdrConfig_.videoFps = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoFps", "I"));
    sdrConfig_.videoDpi = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoDpi", "I"));
    sdrConfig_.marginWidth = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "marginWidth", "I"));
    sdrConfig_.marginHeight = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "marginHeight", "I"));
    sdrConfig_.pixelAspectE4 = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "pixelAspectE4", "I"));
    sdrConfig_.driverPosition = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "driverPosition", "I"));

    // Read string fields
    auto readString = [&](const char* field) -> std::string {
        jfieldID fid = env->GetFieldID(sdrClass, field, "Ljava/lang/String;");
        auto jstr = static_cast<jstring>(env->GetObjectField(sdrConfig, fid));
        if (!jstr) return "";
        const char* chars = env->GetStringUTFChars(jstr, nullptr);
        std::string result(chars);
        env->ReleaseStringUTFChars(jstr, chars);
        env->DeleteLocalRef(jstr);
        return result;
    };
    sdrConfig_.btMac = readString("btMacAddress");
    sdrConfig_.vehicleMake = readString("vehicleMake");
    sdrConfig_.vehicleModel = readString("vehicleModel");
    sdrConfig_.vehicleYear = readString("vehicleYear");
    sdrConfig_.hideClock = env->GetBooleanField(sdrConfig, env->GetFieldID(sdrClass, "hideClock", "Z"));
    sdrConfig_.hideSignal = env->GetBooleanField(sdrConfig, env->GetFieldID(sdrClass, "hideSignal", "Z"));
    sdrConfig_.hideBattery = env->GetBooleanField(sdrConfig, env->GetFieldID(sdrClass, "hideBattery", "Z"));
    env->DeleteLocalRef(sdrClass);

    LOGI("Starting session: video=%dx%d@%dfps dpi=%d",
         sdrConfig_.videoWidth, sdrConfig_.videoHeight,
         sdrConfig_.videoFps, sdrConfig_.videoDpi);

    // Create JNI transport from the Nearby stream pipe
    transport_ = std::make_shared<JniTransport>(*ioService_, jvm_, transportRef);
    rawTransport_ = transport_;

    // Build aasdk pipeline on the io_service thread
    ioService_->post([this, self = shared_from_this()]() {
        // 1. SSL + Cryptor
        LOGI("Creating SSL cryptor...");
        auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
        cryptor_ = std::make_shared<aasdk::messenger::Cryptor>(sslWrapper);
        try {
            cryptor_->init();
            LOGI("Cryptor initialized OK (connect mode)");
        } catch (const std::exception& e) {
            LOGE("Cryptor init FAILED: %s", e.what());
            if (cbMethods_.onError && callbackRef_) {
                bool attached;
                JNIEnv* env = getEnv(attached);
                if (env) {
                    jstring msg = env->NewStringUTF(e.what());
                    env->CallVoidMethod(callbackRef_, cbMethods_.onError, msg);
                    env->DeleteLocalRef(msg);
                }
                releaseEnv(attached);
            }
            return;
        }

        // 2. Messenger
        LOGI("Creating messenger...");
        auto inStream = std::make_shared<aasdk::messenger::MessageInStream>(
            *ioService_, rawTransport_, cryptor_);
        auto outStream = std::make_shared<aasdk::messenger::MessageOutStream>(
            *ioService_, rawTransport_, cryptor_);
        messenger_ = std::make_shared<aasdk::messenger::Messenger>(
            *ioService_, inStream, outStream);

        // 3. Control channel
        controlChannel_ = std::make_shared<aasdk::channel::control::ControlServiceChannel>(
            *strand_, messenger_);

        // 4. Service channels (created now, started after SDR)
        videoChannel_ = std::make_shared<aasdk::channel::mediasink::video::VideoMediaSinkService>(
            *strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO);
        mediaAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::MediaAudioChannel>(
            *strand_, messenger_);
        guidanceAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::GuidanceAudioChannel>(
            *strand_, messenger_);
        systemAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>(
            *strand_, messenger_);
        telephonyAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::TelephonyAudioChannel>(
            *strand_, messenger_);
        inputChannel_ = std::make_shared<aasdk::channel::inputsource::InputSourceService>(
            *strand_, messenger_);
        sensorChannel_ = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
            *strand_, messenger_);

        micChannel_ = std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
            *strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE);

        // Nav/media/phone status channels
        navChannel_ = std::make_shared<aasdk::channel::navigationstatus::NavigationStatusService>(
            *strand_, messenger_);
        mediaStatusChannel_ = std::make_shared<aasdk::channel::mediaplaybackstatus::MediaPlaybackStatusService>(
            *strand_, messenger_);
        phoneStatusChannel_ = std::make_shared<aasdk::channel::phonestatus::PhoneStatusService>(
            *strand_, messenger_);

        // Bluetooth channel (only if BT MAC configured)
        if (!sdrConfig_.btMac.empty()) {
            bluetoothChannel_ = std::make_shared<aasdk::channel::bluetooth::BluetoothService>(
                *strand_, messenger_);
        }

        // 5. Initiate version exchange (request v1.7 — DHU uses 1.7, aasdk defaults to 1.6)
        LOGI("Sending version request (v1.7)...");
        {
            auto message = std::make_shared<aasdk::messenger::Message>(
                aasdk::messenger::ChannelId::CONTROL,
                aasdk::messenger::EncryptionType::PLAIN,
                aasdk::messenger::MessageType::SPECIFIC);
            message->insertPayload(
                aasdk::messenger::MessageId(
                    aap_protobuf::service::control::message::ControlMessageType::MESSAGE_VERSION_REQUEST).getData());
            aasdk::common::Data versionBuffer(4, 0);
            uint16_t major = 1, minor = 7;
            versionBuffer[0] = (major >> 8) & 0xFF;
            versionBuffer[1] = major & 0xFF;
            versionBuffer[2] = (minor >> 8) & 0xFF;
            versionBuffer[3] = minor & 0xFF;
            message->insertPayload(versionBuffer);
            auto promise = aasdk::channel::SendPromise::defer(*strand_);
            promise->then([]() {},
                [this](const auto& e) { this->onChannelError(e); });
            controlChannel_->send(std::move(message), std::move(promise));
        }
        controlChannel_->receive(shared_from_this());
    });
}

// ============================================================================
// stop()
// ============================================================================

void JniSession::stop()
{
    if (stopped_.exchange(true)) return;
    LOGI("Stopping session");
    streaming_ = false;

    if (transport_) transport_->stop();
    if (messenger_) messenger_->stop();

    ioWork_.reset();
    ioService_->stop();
    if (ioThread_.joinable()) ioThread_.join();

    // Notify Kotlin
    if (callbackRef_ && cbMethods_.onSessionStopped) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            jstring reason = env->NewStringUTF("stopped");
            env->CallVoidMethod(callbackRef_, cbMethods_.onSessionStopped, reason);
            env->DeleteLocalRef(reason);
            env->DeleteGlobalRef(callbackRef_);
            callbackRef_ = nullptr;
        }
        releaseEnv(attached);
    }
    transport_.reset();
}

// ============================================================================
// IControlServiceChannelEventHandler Ã¢â‚¬â€ AA handshake + session control
// ============================================================================

void JniSession::onVersionResponse(uint16_t majorCode, uint16_t minorCode,
                                    aap_protobuf::shared::MessageStatus status)
{
    LOGI("Version response: %d.%d status=%d", majorCode, minorCode, static_cast<int>(status));

    try {
        cryptor_->doHandshake();
        auto hsData = cryptor_->readHandshakeBuffer();
        if (!hsData.empty()) {
            auto promise = aasdk::channel::SendPromise::defer(*strand_);
            promise->then([]() {},
                [this](const auto& e) { this->onChannelError(e); });
            controlChannel_->sendHandshake(std::move(hsData), std::move(promise));
        }
    } catch (const std::exception& e) {
        LOGE("Handshake initiation failed: %s", e.what());
    }
    controlChannel_->receive(shared_from_this());
}

void JniSession::onHandshake(const aasdk::common::DataConstBuffer& payload)
{
    LOGI("Handshake data received (%zu bytes)", payload.size);

    try {
        cryptor_->writeHandshakeBuffer(payload);
        auto complete = cryptor_->doHandshake();

        if (complete) {
            LOGI("TLS handshake complete Ã¢â‚¬â€ sending AuthComplete");
            aap_protobuf::service::control::message::AuthResponse authResponse;
            authResponse.set_status(0);
            auto promise = aasdk::channel::SendPromise::defer(*strand_);
            promise->then([]() {},
                [this](const auto& e) { this->onChannelError(e); });
            controlChannel_->sendAuthComplete(authResponse, std::move(promise));
        } else {
            auto hsData = cryptor_->readHandshakeBuffer();
            if (!hsData.empty()) {
                auto promise = aasdk::channel::SendPromise::defer(*strand_);
                promise->then([]() {},
                    [this](const auto& e) { this->onChannelError(e); });
                controlChannel_->sendHandshake(std::move(hsData), std::move(promise));
            }
        }
    } catch (const std::exception& e) {
        LOGE("Handshake processing failed: %s", e.what());
    }
    controlChannel_->receive(shared_from_this());
}

void JniSession::onServiceDiscoveryRequest(
    const aap_protobuf::service::control::message::ServiceDiscoveryRequest& /*request*/)
{
    LOGI("Service discovery request Ã¢â‚¬â€ building response");

    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    buildServiceDiscoveryResponse(response);

    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then(
        [this]() {
            LOGI("SDR sent Ã¢â‚¬â€ starting all service handlers");
            startAllHandlers();
            streaming_ = true;
            callVoidCallback(cbMethods_.onSessionStarted);
        },
        [this](const auto& e) { this->onChannelError(e); });

    controlChannel_->sendServiceDiscoveryResponse(response, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void JniSession::onAudioFocusRequest(
    const aap_protobuf::service::control::message::AudioFocusRequest& request)
{
    auto focus_type = request.audio_focus_type();
    // Match bridge logic: map request type → response state
    aap_protobuf::service::control::message::AudioFocusStateType state;
    switch (focus_type) {
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN;
            break;
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN_TRANSIENT:
        case aap_protobuf::service::control::message::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
            break;
        case aap_protobuf::service::control::message::AUDIO_FOCUS_RELEASE:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_LOSS;
            break;
        default:
            state = aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_LOSS;
            break;
    }
    LOGI("Audio focus request: type=%d → state=%d", (int)focus_type, (int)state);

    aap_protobuf::service::control::message::AudioFocusNotification response;
    response.set_focus_state(state);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    controlChannel_->sendAudioFocusResponse(response, std::move(promise));
    controlChannel_->receive(shared_from_this());

    if (cbMethods_.onAudioFocusRequest) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            env->CallVoidMethod(callbackRef_, cbMethods_.onAudioFocusRequest,
                                static_cast<jint>(request.audio_focus_type()));
        }
        releaseEnv(attached);
    }
}

void JniSession::sendUnsolicitedAudioFocusGain()
{
    LOGI("Sending unsolicited AUDIO_FOCUS_STATE_GAIN");
    aap_protobuf::service::control::message::AudioFocusNotification notification;
    notification.set_focus_state(aap_protobuf::service::control::message::AUDIO_FOCUS_STATE_GAIN);
    notification.set_unsolicited(true);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    controlChannel_->sendAudioFocusResponse(notification, std::move(promise));
}

void JniSession::onNavigationFocusRequest(
    const aap_protobuf::service::control::message::NavFocusRequestNotification& /*request*/)
{
    LOGI("Navigation focus request Ã¢â‚¬â€ granting");
    aap_protobuf::service::control::message::NavFocusNotification response;
    response.set_focus_type(aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    controlChannel_->sendNavigationFocusResponse(response, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void JniSession::onByeByeRequest(
    const aap_protobuf::service::control::message::ByeByeRequest& /*request*/)
{
    LOGI("ByeBye request Ã¢â‚¬â€ disconnecting");
    aap_protobuf::service::control::message::ByeByeResponse response;
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([this]() { stop(); }, [this](const auto&) { stop(); });
    controlChannel_->sendShutdownResponse(response, std::move(promise));
}

void JniSession::onByeByeResponse(
    const aap_protobuf::service::control::message::ByeByeResponse& /*response*/)
{
    LOGI("ByeBye response received");
    stop();
}

void JniSession::onBatteryStatusNotification(
    const aap_protobuf::service::control::message::BatteryStatusNotification& notification)
{
    int level = notification.battery_level();
    bool charging = notification.critical_battery();

    if (cbMethods_.onPhoneBattery) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            env->CallVoidMethod(callbackRef_, cbMethods_.onPhoneBattery,
                                static_cast<jint>(level), static_cast<jboolean>(charging));
        }
        releaseEnv(attached);
    }
    controlChannel_->receive(shared_from_this());
}

void JniSession::onVoiceSessionRequest(
    const aap_protobuf::service::control::message::VoiceSessionNotification& request)
{
    bool active = request.status() == aap_protobuf::service::control::message::VOICE_SESSION_START;

    if (cbMethods_.onVoiceSession) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            env->CallVoidMethod(callbackRef_, cbMethods_.onVoiceSession,
                                static_cast<jboolean>(active));
        }
        releaseEnv(attached);
    }
    controlChannel_->receive(shared_from_this());
}

void JniSession::onPingRequest(
    const aap_protobuf::service::control::message::PingRequest& request)
{
    aap_protobuf::service::control::message::PingResponse response;
    response.set_timestamp(request.timestamp());
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [](const auto&) {});
    controlChannel_->sendPingResponse(response, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void JniSession::onPingResponse(
    const aap_protobuf::service::control::message::PingResponse& /*response*/)
{
    pingOutstanding_ = false;
    controlChannel_->receive(shared_from_this());
}

void JniSession::sendPing()
{
    if (stopped_ || !controlChannel_) return;
    pingOutstanding_ = true;
    aap_protobuf::service::control::message::PingRequest request;
    request.set_timestamp(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    controlChannel_->sendPingRequest(request, std::move(promise));
}

void JniSession::schedulePing()
{
    if (stopped_) return;
    auto timer = std::make_shared<boost::asio::deadline_timer>(
        *ioService_, boost::posix_time::milliseconds(1500));
    timer->async_wait([this, timer, self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec || stopped_) return;
        if (!pingOutstanding_) {
            sendPing();
        }
        schedulePing();
    });
}

void JniSession::onChannelError(const aasdk::error::Error& e)
{
    LOGE("Channel error: %s", e.what());
    if (cbMethods_.onError) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            jstring msg = env->NewStringUTF(e.what());
            env->CallVoidMethod(callbackRef_, cbMethods_.onError, msg);
            env->DeleteLocalRef(msg);
        }
        releaseEnv(attached);
    }
}

// ============================================================================
// IVideoMediaSinkServiceEventHandler Ã¢â‚¬â€ video from phone
// ============================================================================

void JniSession::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Video channel open request");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_SUCCESS);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    videoChannel_->sendChannelOpenResponse(response, std::move(promise));
    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup& request)
{
    LOGI("Video setup: type=%d", request.type());

    aap_protobuf::service::media::shared::message::Config config;
    config.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config.set_max_unacked(30);
    config.add_configuration_indices(0);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    videoChannel_->sendChannelSetupResponse(config, std::move(promise));
    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaChannelStartIndication(
    const aap_protobuf::service::media::shared::message::Start& /*indication*/)
{
    LOGI("Video stream starting");

    aap_protobuf::service::media::video::message::VideoFocusNotification focus;
    focus.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    focus.set_unsolicited(false);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    videoChannel_->sendVideoFocusIndication(focus, std::move(promise));

    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaChannelStopIndication(
    const aap_protobuf::service::media::shared::message::Stop& /*indication*/)
{
    LOGI("Video stream stopping");
    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaWithTimestampIndication(
    aasdk::messenger::Timestamp::ValueType timestamp,
    const aasdk::common::DataConstBuffer& buffer)
{
    // Hot path Ã¢â‚¬â€ video frame. Send ACK immediately (flow control).
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(0);
    ack.set_ack(1);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [](const auto&) {});
    videoChannel_->sendMediaAckIndication(ack, std::move(promise));

    // Detect IDR (keyframe) from H.264/H.265 NAL headers
    bool isKeyFrame = false;
    if (buffer.size >= 5) {
        const uint8_t* d = buffer.cdata;
        // Find start code (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01)
        size_t offset = 0;
        if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 1) offset = 4;
        else if (d[0] == 0 && d[1] == 0 && d[2] == 1) offset = 3;
        if (offset > 0 && offset < buffer.size) {
            uint8_t nalType = d[offset] & 0x1F; // H.264 NAL type
            if (nalType == 5 || nalType == 7) { // IDR slice or SPS
                isKeyFrame = true;
            }
            // H.265: NAL type is bits 1-6 of first byte
            uint8_t hevcNalType = (d[offset] >> 1) & 0x3F;
            if (hevcNalType >= 16 && hevcNalType <= 21) { // IRAP NAL types
                isKeyFrame = true;
            }
        }
    }

    // Dispatch to Kotlin
    if (cbMethods_.onVideoFrame && callbackRef_) {
        bool attached;
        JNIEnv* env = getEnv(attached);
        if (env) {
            jbyteArray jdata = env->NewByteArray(static_cast<jsize>(buffer.size));
            env->SetByteArrayRegion(jdata, 0, static_cast<jsize>(buffer.size),
                                    reinterpret_cast<const jbyte*>(buffer.cdata));
            env->CallVoidMethod(callbackRef_, cbMethods_.onVideoFrame,
                                jdata, static_cast<jlong>(timestamp),
                                static_cast<jint>(sdrConfig_.videoWidth),
                                static_cast<jint>(sdrConfig_.videoHeight),
                                static_cast<jboolean>(isKeyFrame));
            env->DeleteLocalRef(jdata);
        }
        releaseEnv(attached);
    }

    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaIndication(const aasdk::common::DataConstBuffer& buffer)
{
    onMediaWithTimestampIndication(0, buffer);
}

void JniSession::onVideoFocusRequest(
    const aap_protobuf::service::media::video::message::VideoFocusRequestNotification& /*request*/)
{
    LOGI("Video focus request from phone");
    videoChannel_->receive(shared_from_this());
}

// ============================================================================
// startAllHandlers() Ã¢â‚¬â€ begin receiving on all service channels
// ============================================================================

void JniSession::startAllHandlers()
{
    LOGI("Starting all service handlers");

    // Audio handlers (3 instances Ã¢â‚¬â€ same class, different channel types)
    mediaAudioHandler_ = std::make_shared<JniAudioSinkHandler>(
        *strand_, mediaAudioChannel_, *this, JniAudioSinkHandler::AudioType::Media);
    mediaAudioHandler_->start();

    guidanceAudioHandler_ = std::make_shared<JniAudioSinkHandler>(
        *strand_, guidanceAudioChannel_, *this, JniAudioSinkHandler::AudioType::Guidance);
    guidanceAudioHandler_->start();

    systemAudioHandler_ = std::make_shared<JniAudioSinkHandler>(
        *strand_, systemAudioChannel_, *this, JniAudioSinkHandler::AudioType::System);
    systemAudioHandler_->start();

    telephonyAudioHandler_ = std::make_shared<JniAudioSinkHandler>(
        *strand_, telephonyAudioChannel_, *this, JniAudioSinkHandler::AudioType::Telephony);
    telephonyAudioHandler_->start();

    // Sensor handler
    sensorHandler_ = std::make_shared<JniSensorHandler>(*strand_, sensorChannel_, *this);
    sensorHandler_->start();

    // Input handler
    inputHandler_ = std::make_shared<JniInputHandler>(*strand_, inputChannel_, *this);
    inputHandler_->start();

    // Mic handler
    micHandler_ = std::make_shared<JniMicHandler>(*strand_, micChannel_, *this);
    micHandler_->start();

    // Navigation status handler
    navHandler_ = std::make_shared<JniNavStatusHandler>(*strand_, navChannel_, *this);
    navHandler_->start();

    // Media playback status handler
    if (mediaStatusChannel_) {
        mediaStatusHandler_ = std::make_shared<JniMediaStatusHandler>(
            *strand_, mediaStatusChannel_, *this);
        mediaStatusHandler_->start();
    }

    // Phone status handler
    if (phoneStatusChannel_) {
        phoneStatusHandler_ = std::make_shared<JniPhoneStatusHandler>(
            *strand_, phoneStatusChannel_, *this);
        phoneStatusHandler_->start();
    }

    // Bluetooth handler
    if (bluetoothChannel_) {
        bluetoothHandler_ = std::make_shared<JniBluetoothHandler>(
            *strand_, bluetoothChannel_, *this);
        bluetoothHandler_->start();
    }

    LOGI("All %d handlers started", 9);

    // Send initial ping (bridge does this — phone expects HU to initiate pings)
    sendPing();
    schedulePing();

    // Don't send VideoFocusIndication here — wait for phone to open the video channel first.
    // The phone's projection state machine expects to control video focus timing.
}

// ============================================================================
// buildServiceDiscoveryResponse() Ã¢â‚¬â€ tell phone what we support
// ============================================================================

void JniSession::buildServiceDiscoveryResponse(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response)
{
    response.mutable_channels()->Reserve(256);

    // v1.6 protocol fields (matches bridge-mode live_session.cpp)
    response.set_driver_position(
        sdrConfig_.driverPosition == 1
            ? aap_protobuf::service::control::message::DRIVER_POSITION_RIGHT
            : aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);
    response.set_display_name("OpenAutoLink");
    response.set_probe_for_support(false);
    response.set_can_play_native_media_during_vr(false);

    // Connection config with ping
    auto* connCfg = response.mutable_connection_configuration();
    auto* ping = connCfg->mutable_ping_configuration();
    ping->set_timeout_ms(5000);
    ping->set_interval_ms(1500);
    ping->set_high_latency_threshold_ms(500);
    ping->set_tracked_ping_count(5);

    // HeadUnitInfo (v1.6)
    auto* hui = response.mutable_headunit_info();
    hui->set_make(sdrConfig_.vehicleMake);
    hui->set_model(sdrConfig_.vehicleModel);
    hui->set_year(sdrConfig_.vehicleYear);
    hui->set_vehicle_id("OAL-JNI-1");
    hui->set_head_unit_make("OpenAutoLink");
    hui->set_head_unit_model("Direct JNI");
    hui->set_head_unit_software_build("1");
    hui->set_head_unit_software_version("1.0");

    // Deprecated fields for backward compat (bridge sets these too)
    response.set_head_unit_make("OpenAutoLink");
    response.set_model(sdrConfig_.vehicleModel);
    response.set_year(sdrConfig_.vehicleYear);
    response.set_vehicle_id("OAL-JNI-1");
    response.set_head_unit_model("Direct JNI");
    response.set_head_unit_software_build("1");
    response.set_head_unit_software_version("1.0");

    // ---- Video channel (matches bridge exactly — NO audio_type) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_VIDEO));
      auto* ms = svc->mutable_media_sink_service();
      ms->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
      ms->set_available_while_in_call(true);

      using VRes = aap_protobuf::service::media::sink::message::VideoCodecResolutionType;
      using VFps = aap_protobuf::service::media::sink::message::VideoFrameRateType;
      auto res = VRes::VIDEO_1920x1080;
      if (sdrConfig_.videoHeight <= 480) res = VRes::VIDEO_800x480;
      else if (sdrConfig_.videoHeight <= 720) res = VRes::VIDEO_1280x720;
      else if (sdrConfig_.videoHeight <= 1080) res = VRes::VIDEO_1920x1080;
      else if (sdrConfig_.videoHeight <= 1440) res = VRes::VIDEO_2560x1440;
      else res = VRes::VIDEO_3840x2160;
      auto fps = sdrConfig_.videoFps >= 60 ? VFps::VIDEO_FPS_60 : VFps::VIDEO_FPS_30;

      // H.265 at all tiers, then H.264 fallback (matches bridge auto-negotiate)
      int tiers[] = {5, 4, 3, 2, 1};
      for (int t : tiers) {
          auto* vc = ms->add_video_configs();
          vc->set_codec_resolution(static_cast<VRes>(t));
          vc->set_frame_rate(fps);
          vc->set_density(sdrConfig_.videoDpi);
          vc->set_video_codec_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H265);
          if (sdrConfig_.marginWidth > 0) vc->set_width_margin(sdrConfig_.marginWidth);
          if (sdrConfig_.marginHeight > 0) vc->set_height_margin(sdrConfig_.marginHeight);
          if (sdrConfig_.pixelAspectE4 > 0) vc->set_pixel_aspect_ratio_e4(sdrConfig_.pixelAspectE4);
      }
      for (int t : {3, 2, 1}) { // H.264 fallback at ≤1080p
          auto* vc = ms->add_video_configs();
          vc->set_codec_resolution(static_cast<VRes>(t));
          vc->set_frame_rate(fps);
          vc->set_density(sdrConfig_.videoDpi);
          vc->set_video_codec_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);
          if (sdrConfig_.marginWidth > 0) vc->set_width_margin(sdrConfig_.marginWidth);
          if (sdrConfig_.marginHeight > 0) vc->set_height_margin(sdrConfig_.marginHeight);
          if (sdrConfig_.pixelAspectE4 > 0) vc->set_pixel_aspect_ratio_e4(sdrConfig_.pixelAspectE4);
      }
    }

    // ---- Media audio (48kHz stereo) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_MEDIA_AUDIO));
      auto* ms = svc->mutable_media_sink_service();
      ms->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
      ms->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_MEDIA);
      ms->set_available_while_in_call(true);
      auto* ac = ms->add_audio_configs();
      ac->set_sampling_rate(48000); ac->set_number_of_bits(16); ac->set_number_of_channels(2);
    }

    // ---- Guidance/Speech audio (16kHz mono) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_GUIDANCE_AUDIO));
      auto* ms = svc->mutable_media_sink_service();
      ms->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
      ms->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_GUIDANCE);
      ms->set_available_while_in_call(true);
      auto* ac = ms->add_audio_configs();
      ac->set_sampling_rate(16000); ac->set_number_of_bits(16); ac->set_number_of_channels(1);
    }

    // ---- System audio (16kHz mono) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_SYSTEM_AUDIO));
      auto* ms = svc->mutable_media_sink_service();
      ms->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
      ms->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_SYSTEM_AUDIO);
      ms->set_available_while_in_call(true);
      auto* ac = ms->add_audio_configs();
      ac->set_sampling_rate(16000); ac->set_number_of_bits(16); ac->set_number_of_channels(1);
    }

    // ---- Telephony audio (16kHz mono) — disabled: crashes AA v16.7 without BT HFP ----
    // { auto* svc = response.add_channels();
    //   svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SINK_TELEPHONY_AUDIO));
    //   auto* ms = svc->mutable_media_sink_service();
    //   ms->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    //   ms->set_audio_type(aap_protobuf::service::media::sink::message::AUDIO_STREAM_TELEPHONY);
    //   ms->set_available_while_in_call(true);
    //   auto* ac = ms->add_audio_configs();
    //   ac->set_sampling_rate(16000); ac->set_number_of_bits(16); ac->set_number_of_channels(1);
    // }

    // ---- Mic input (16kHz mono) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE));
      auto* msrc = svc->mutable_media_source_service();
      msrc->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
      msrc->set_available_while_in_call(true);
      auto* ac = msrc->mutable_audio_config();
      ac->set_sampling_rate(16000); ac->set_number_of_bits(16); ac->set_number_of_channels(1);
    }

    // ---- Sensor channel ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::SENSOR));
      namespace ST = aap_protobuf::service::sensorsource::message;
      auto* ss = svc->mutable_sensor_source_service();
      ss->add_sensors()->set_sensor_type(ST::SENSOR_DRIVING_STATUS_DATA);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_LOCATION);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_NIGHT_MODE);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_SPEED);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_GEAR);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_PARKING_BRAKE);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_FUEL);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_ODOMETER);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_ENVIRONMENT_DATA);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_COMPASS);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_ACCELEROMETER_DATA);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_GYROSCOPE_DATA);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_GPS_SATELLITE_DATA);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_RPM);
      ss->add_sensors()->set_sensor_type(ST::SENSOR_VEHICLE_ENERGY_MODEL);
    }

    // ---- Input channel ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::INPUT_SOURCE));
      auto* is = svc->mutable_input_source_service();
      for (int kc : {84, 85, 86, 87, 88, 89, 90, 126, 127}) {
          is->add_keycodes_supported(kc);
      }
      auto* ts = is->add_touchscreen();
      ts->set_width(sdrConfig_.videoWidth);
      ts->set_height(sdrConfig_.videoHeight);
      ts->set_type(aap_protobuf::service::inputsource::message::CAPACITIVE);
    }

    // ---- Bluetooth channel ----
    if (!sdrConfig_.btMac.empty()) {
        auto* svc = response.add_channels();
        svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::BLUETOOTH));
        auto* bs = svc->mutable_bluetooth_service();
        bs->set_car_address(sdrConfig_.btMac);
        bs->add_supported_pairing_methods(
            aap_protobuf::service::bluetooth::message::BLUETOOTH_PAIRING_PIN);
        bs->add_supported_pairing_methods(
            aap_protobuf::service::bluetooth::message::BLUETOOTH_PAIRING_NUMERIC_COMPARISON);
    }

    // ---- Navigation Status (IMAGE mode, matches bridge) ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::NAVIGATION_STATUS));
      auto* ns = svc->mutable_navigation_status_service();
      ns->set_minimum_interval_ms(500);
      ns->set_type(aap_protobuf::service::navigationstatus::NavigationStatusService::IMAGE);
      auto* img = ns->mutable_image_options();
      img->set_width(256);
      img->set_height(256);
      img->set_colour_depth_bits(32);
    }

    // ---- Media Playback Status ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::MEDIA_PLAYBACK_STATUS));
      svc->mutable_media_playback_service();
    }

    // ---- Phone Status ----
    { auto* svc = response.add_channels();
      svc->set_id(static_cast<int32_t>(aasdk::messenger::ChannelId::PHONE_STATUS));
      svc->mutable_phone_status_service();
    }

    LOGI("SDR built: %d channels, %d bytes", response.channels_size(),
         static_cast<int>(response.ByteSizeLong()));

    // Debug: dump each channel's key fields
    for (int i = 0; i < response.channels_size(); i++) {
        const auto& ch = response.channels(i);
        if (ch.has_media_sink_service()) {
            const auto& ms = ch.media_sink_service();
            LOGI("  ch[%d] id=%d media_sink: avail_type=%d audio_type=%d "
                 "video_configs=%d audio_configs=%d in_call=%d",
                 i, ch.id(), (int)ms.available_type(),
                 ms.has_audio_type() ? (int)ms.audio_type() : -1,
                 ms.video_configs_size(), ms.audio_configs_size(),
                 (int)ms.available_while_in_call());
        } else if (ch.has_media_source_service()) {
            LOGI("  ch[%d] id=%d media_source: avail_type=%d", i, ch.id(),
                 (int)ch.media_source_service().available_type());
        } else if (ch.has_sensor_source_service()) {
            LOGI("  ch[%d] id=%d sensor: %d types", i, ch.id(),
                 ch.sensor_source_service().sensors_size());
        } else if (ch.has_input_source_service()) {
            LOGI("  ch[%d] id=%d input: %d keycodes, %d touch", i, ch.id(),
                 ch.input_source_service().keycodes_supported_size(),
                 ch.input_source_service().touchscreen_size());
        } else if (ch.has_navigation_status_service()) {
            LOGI("  ch[%d] id=%d nav_status: interval=%d type=%d", i, ch.id(),
                 ch.navigation_status_service().minimum_interval_ms(),
                 (int)ch.navigation_status_service().type());
        } else {
            LOGI("  ch[%d] id=%d (other service)", i, ch.id());
        }
    }
}

// ============================================================================
// Input forwarding (app Ã¢â€ â€™ phone)
// ============================================================================

void JniSession::sendTouchEvent(int action, int pointerId, float x, float y, int pointerCount)
{
    if (!streaming_ || !inputChannel_) return;
    ioService_->post([this, action, pointerId, x, y, pointerCount]() {
        aap_protobuf::service::inputsource::message::InputReport report;

        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        report.set_timestamp(static_cast<uint64_t>(now));

        auto* touch = report.mutable_touch_event();
        touch->set_action(
            static_cast<aap_protobuf::service::inputsource::message::PointerAction>(action));
        touch->set_action_index(0);

        auto* ptr = touch->add_pointer_data();
        ptr->set_x(static_cast<uint32_t>(x));
        ptr->set_y(static_cast<uint32_t>(y));
        ptr->set_pointer_id(static_cast<uint32_t>(pointerId));

        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        inputChannel_->sendInputReport(report, std::move(promise));
    });
}

void JniSession::sendKeyEvent(int keyCode, bool isDown)
{
    if (!streaming_ || !inputChannel_) return;
    ioService_->post([this, keyCode, isDown]() {
        aap_protobuf::service::inputsource::message::InputReport report;

        auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        report.set_timestamp(static_cast<uint64_t>(now));

        auto* keyEvent = report.mutable_key_event();
        auto* key = keyEvent->add_keys();
        key->set_keycode(static_cast<uint32_t>(keyCode));
        key->set_down(isDown);
        key->set_metastate(0);
        key->set_longpress(false);

        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        inputChannel_->sendInputReport(report, std::move(promise));
    });
}

void JniSession::sendGpsLocation(double lat, double lon, double alt,
                                  float speed, float bearing, long long timestampMs)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, lat, lon, alt, speed, bearing, timestampMs]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* gps = batch.add_location_data();
        gps->set_latitude_e7(static_cast<int32_t>(lat * 1e7));
        gps->set_longitude_e7(static_cast<int32_t>(lon * 1e7));
        gps->set_altitude_e2(static_cast<int32_t>(alt * 1e2));
        gps->set_speed_e3(static_cast<int32_t>(speed * 1000));
        gps->set_bearing_e6(static_cast<int32_t>(bearing * 1e6));
        gps->set_timestamp(static_cast<uint64_t>(timestampMs));
        gps->set_accuracy_e3(static_cast<uint32_t>(10 * 1000));

        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendVehicleSensor(int sensorType, const uint8_t* data, size_t length)
{
    if (!streaming_ || !sensorChannel_) return;
    // sensorType maps to SensorBatch field numbers:
    // Vehicle data is sent as pre-serialized SensorBatch protobuf from Kotlin.
    std::vector<uint8_t> dataCopy(data, data + length);
    ioService_->post([this, sensorType, dataCopy = std::move(dataCopy)]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        if (batch.ParseFromArray(dataCopy.data(), static_cast<int>(dataCopy.size()))) {
            auto promise = aasdk::channel::SendPromise::defer(*strand_);
            promise->then([]() {}, [](const auto&) {});
            sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
        }
    });
}

void JniSession::sendMicAudio(const uint8_t* data, size_t length)
{
    if (!streaming_ || !micHandler_ || !micHandler_->isOpen()) return;
    std::vector<uint8_t> dataCopy(data, data + length);
    ioService_->post([this, dataCopy = std::move(dataCopy)]() {
        micHandler_->feedAudio(dataCopy.data(), dataCopy.size());
    });
}

void JniSession::requestKeyframe()
{
    if (!streaming_ || !videoChannel_) return;
    ioService_->post([this]() {
        LOGI("Requesting keyframe (VideoFocusIndication)");
        aap_protobuf::service::media::video::message::VideoFocusNotification focus;
        focus.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
        focus.set_unsolicited(false);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        videoChannel_->sendVideoFocusIndication(focus, std::move(promise));
    });
}

// ============================================================================
// Typed vehicle sensor methods Ã¢â‚¬â€ each builds SensorBatch and sends
// ============================================================================

void JniSession::sendSpeedSensor(int speedMmPerS)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, speedMmPerS]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* sd = batch.add_speed_data();
        sd->set_speed_e3(speedMmPerS);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendGearSensor(int gear)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, gear]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* gd = batch.add_gear_data();
        gd->set_gear(static_cast<aap_protobuf::service::sensorsource::message::Gear>(gear));
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendParkingBrakeSensor(bool engaged)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, engaged]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* pb = batch.add_parking_brake_data();
        pb->set_parking_brake(engaged);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendNightModeSensor(bool night)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, night]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* nm = batch.add_night_mode_data();
        nm->set_night_mode(night);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendDrivingStatusSensor(bool moving)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, moving]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* ds = batch.add_driving_status_data();
        ds->set_status(moving ? 31 : 0);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendFuelSensor(int levelPct, int rangeM, bool lowFuel)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, levelPct, rangeM, lowFuel]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* fd = batch.add_fuel_data();
        fd->set_fuel_level(levelPct);
        fd->set_range(rangeM);
        fd->set_low_fuel_warning(lowFuel);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendAccelerometerSensor(int xE3, int yE3, int zE3)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, xE3, yE3, zE3]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* ad = batch.add_accelerometer_data();
        ad->set_acceleration_x_e3(xE3);
        ad->set_acceleration_y_e3(yE3);
        ad->set_acceleration_z_e3(zE3);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendGyroscopeSensor(int rxE3, int ryE3, int rzE3)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, rxE3, ryE3, rzE3]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* gd = batch.add_gyroscope_data();
        gd->set_rotation_speed_x_e3(rxE3);
        gd->set_rotation_speed_y_e3(ryE3);
        gd->set_rotation_speed_z_e3(rzE3);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendCompassSensor(int bearingE6, int pitchE6, int rollE6)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, bearingE6, pitchE6, rollE6]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* cd = batch.add_compass_data();
        cd->set_bearing_e6(bearingE6);
        cd->set_pitch_e6(pitchE6);
        cd->set_roll_e6(rollE6);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

void JniSession::sendRpmSensor(int rpmE3)
{
    if (!streaming_ || !sensorChannel_) return;
    ioService_->post([this, rpmE3]() {
        aap_protobuf::service::sensorsource::message::SensorBatch batch;
        auto* rd = batch.add_rpm_data();
        rd->set_rpm_e3(rpmE3);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        sensorChannel_->sendSensorEventIndication(batch, std::move(promise));
    });
}

// ============================================================================
// Dispatch methods Ã¢â‚¬â€ called by handler classes to fire JNI callbacks
// ============================================================================

void JniSession::dispatchAudioFrame(const uint8_t* data, size_t size,
                                     int purpose, int sampleRate, int channels)
{
    if (!cbMethods_.onAudioFrame || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        jbyteArray jdata = env->NewByteArray(static_cast<jsize>(size));
        env->SetByteArrayRegion(jdata, 0, static_cast<jsize>(size),
                                reinterpret_cast<const jbyte*>(data));
        env->CallVoidMethod(callbackRef_, cbMethods_.onAudioFrame,
                            jdata, static_cast<jint>(purpose),
                            static_cast<jint>(sampleRate),
                            static_cast<jint>(channels));
        env->DeleteLocalRef(jdata);
    }
    releaseEnv(attached);
}

void JniSession::dispatchMicRequest(bool open)
{
    if (!cbMethods_.onMicRequest || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        env->CallVoidMethod(callbackRef_, cbMethods_.onMicRequest,
                            static_cast<jboolean>(open));
    }
    releaseEnv(attached);
}

void JniSession::dispatchNavStatus(int status)
{
    if (!cbMethods_.onNavigationStatus || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        env->CallVoidMethod(callbackRef_, cbMethods_.onNavigationStatus,
                            static_cast<jint>(status));
    }
    releaseEnv(attached);
}

void JniSession::dispatchNavTurn(const std::string& maneuver, const std::string& road,
                                  const uint8_t* iconData, size_t iconSize)
{
    if (!cbMethods_.onNavigationTurn || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        jstring jmaneuver = env->NewStringUTF(maneuver.c_str());
        jstring jroad = env->NewStringUTF(road.c_str());
        jbyteArray jicon = nullptr;
        if (iconData && iconSize > 0) {
            jicon = env->NewByteArray(static_cast<jsize>(iconSize));
            env->SetByteArrayRegion(jicon, 0, static_cast<jsize>(iconSize),
                                    reinterpret_cast<const jbyte*>(iconData));
        }
        env->CallVoidMethod(callbackRef_, cbMethods_.onNavigationTurn,
                            jmaneuver, jroad, jicon);
        env->DeleteLocalRef(jmaneuver);
        env->DeleteLocalRef(jroad);
        if (jicon) env->DeleteLocalRef(jicon);
    }
    releaseEnv(attached);
}

void JniSession::dispatchNavDistance(int distanceMeters, int etaSeconds,
                                     const std::string& displayDistance,
                                     const std::string& displayUnit)
{
    if (!cbMethods_.onNavigationDistance || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        jstring jdist = displayDistance.empty() ? nullptr : env->NewStringUTF(displayDistance.c_str());
        jstring junit = displayUnit.empty() ? nullptr : env->NewStringUTF(displayUnit.c_str());
        env->CallVoidMethod(callbackRef_, cbMethods_.onNavigationDistance,
                            static_cast<jint>(distanceMeters),
                            static_cast<jint>(etaSeconds),
                            jdist, junit);
        if (jdist) env->DeleteLocalRef(jdist);
        if (junit) env->DeleteLocalRef(junit);
    }
    releaseEnv(attached);
}

void JniSession::dispatchMediaMetadata(const std::string& title, const std::string& artist,
                                        const std::string& album,
                                        const uint8_t* artData, size_t artSize)
{
    if (!cbMethods_.onMediaMetadata || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        jstring jtitle = env->NewStringUTF(title.c_str());
        jstring jartist = env->NewStringUTF(artist.c_str());
        jstring jalbum = env->NewStringUTF(album.c_str());
        jbyteArray jart = nullptr;
        if (artData && artSize > 0) {
            jart = env->NewByteArray(static_cast<jsize>(artSize));
            env->SetByteArrayRegion(jart, 0, static_cast<jsize>(artSize),
                                    reinterpret_cast<const jbyte*>(artData));
        }
        env->CallVoidMethod(callbackRef_, cbMethods_.onMediaMetadata,
                            jtitle, jartist, jalbum, jart);
        env->DeleteLocalRef(jtitle);
        env->DeleteLocalRef(jartist);
        env->DeleteLocalRef(jalbum);
        if (jart) env->DeleteLocalRef(jart);
    }
    releaseEnv(attached);
}

void JniSession::dispatchMediaPlayback(int state, long long positionMs)
{
    if (!cbMethods_.onMediaPlayback || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        env->CallVoidMethod(callbackRef_, cbMethods_.onMediaPlayback,
                            static_cast<jint>(state),
                            static_cast<jlong>(positionMs));
    }
    releaseEnv(attached);
}

void JniSession::dispatchPhoneStatus(int signalStrength, int callState)
{
    if (!cbMethods_.onPhoneStatus || !callbackRef_) return;
    bool attached;
    JNIEnv* env = getEnv(attached);
    if (env) {
        env->CallVoidMethod(callbackRef_, cbMethods_.onPhoneStatus,
                            static_cast<jint>(signalStrength),
                            static_cast<jint>(callState));
    }
    releaseEnv(attached);
}

} // namespace openautolink::jni
