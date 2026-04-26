/*
 * jni_session.cpp — aasdk session lifecycle + JNI callback dispatch.
 *
 * This is the core integration: creates the aasdk pipeline, implements
 * all AA channel event handlers, and fires JNI callbacks to Kotlin.
 *
 * Based on bridge/openautolink/headless/src/live_session.cpp — same
 * aasdk API calls, but output goes to JNI instead of OAL TCP.
 */
#include "jni_session.h"
#include "jni_transport.h"

#include <android/log.h>

// Protobuf messages for SDR building
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>
#include <aap_protobuf/service/control/message/ChannelOpenResponse.pb.h>
#include <aap_protobuf/service/control/message/AuthResponse.pb.h>
#include <aap_protobuf/service/control/message/AudioFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/NavFocusNotification.pb.h>
#include <aap_protobuf/service/control/message/PingResponse.pb.h>
#include <aap_protobuf/service/control/message/ByeByeResponse.pb.h>
#include <aap_protobuf/service/media/shared/message/Config.pb.h>
#include <aap_protobuf/service/media/source/message/Ack.pb.h>
#include <aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h>
#include <aap_protobuf/shared/ChannelDescriptorData.pb.h>
#include <aap_protobuf/shared/VideoConfigData.pb.h>
#include <aap_protobuf/shared/AudioConfigData.pb.h>
#include <aap_protobuf/shared/SensorChannelData.pb.h>
#include <aap_protobuf/shared/InputChannelData.pb.h>
#include <aap_protobuf/shared/BluetoothChannelData.pb.h>
#include <aap_protobuf/shared/TouchConfigData.pb.h>
#include <aap_protobuf/shared/SensorTypeEnum.pb.h>
#include <aap_protobuf/shared/VideoFPSEnum.pb.h>
#include <aap_protobuf/shared/VideoResolutionEnum.pb.h>
#include <aap_protobuf/shared/AVStreamTypeEnum.pb.h>
#include <aap_protobuf/shared/AudioTypeEnum.pb.h>
#include <aap_protobuf/shared/BluetoothPairingMethodEnum.pb.h>

#include <aap_protobuf/service/inputsource/message/InputReport.pb.h>
#include <aap_protobuf/service/inputsource/message/TouchEvent.pb.h>
#include <aap_protobuf/service/inputsource/message/KeyEvent.pb.h>
#include <aap_protobuf/service/sensorsource/message/SensorBatch.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneRequest.pb.h>
#include <aap_protobuf/service/media/source/message/MicrophoneResponse.pb.h>

#include <aasdk/Channel/Promise.hpp>
#include <aasdk/Messenger/ChannelId.hpp>

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
    strand_ = new boost::asio::io_service::strand(*ioService_);
    ioThread_ = std::thread(&JniSession::ioServiceThreadFunc, this);
    LOGI("JniSession created");
}

JniSession::~JniSession()
{
    stop();
    delete strand_;
    strand_ = nullptr;
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
// start() — build the aasdk pipeline
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
    cbMethods_.onVideoFrame = env->GetMethodID(cbClass, "onVideoFrame", "([BJII)V");
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
            callVoidCallback(cbMethods_.onError);
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
        videoChannel_ = std::make_shared<aasdk::channel::mediasink::video::channel::VideoChannel>(
            *strand_, messenger_);
        mediaAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::MediaAudioChannel>(
            *strand_, messenger_);
        guidanceAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::GuidanceAudioChannel>(
            *strand_, messenger_);
        systemAudioChannel_ = std::make_shared<aasdk::channel::mediasink::audio::channel::SystemAudioChannel>(
            *strand_, messenger_);
        inputChannel_ = std::make_shared<aasdk::channel::inputsource::InputSourceService>(
            *strand_, messenger_);
        sensorChannel_ = std::make_shared<aasdk::channel::sensorsource::SensorSourceService>(
            *strand_, messenger_);

        micChannel_ = std::make_shared<aasdk::channel::mediasource::MediaSourceService>(
            *strand_, messenger_, aasdk::messenger::ChannelId::MEDIA_SOURCE_MICROPHONE);

        // 5. Initiate version exchange
        LOGI("Sending version request...");
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {},
            [this](const auto& e) { this->onChannelError(e); });
        controlChannel_->sendVersionRequest(std::move(promise));
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
// IControlServiceChannelEventHandler — AA handshake + session control
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
            LOGI("TLS handshake complete — sending AuthComplete");
            aap_protobuf::service::control::message::AuthResponse authResponse;
            authResponse.set_status(aap_protobuf::shared::AUTH_STATUS_OK);
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
    LOGI("Service discovery request — building response");

    aap_protobuf::service::control::message::ServiceDiscoveryResponse response;
    buildServiceDiscoveryResponse(response);

    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then(
        [this]() {
            LOGI("SDR sent — starting all service handlers");
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
    LOGI("Audio focus request: type=%d", request.audio_focus_type());

    aap_protobuf::service::control::message::AudioFocusNotification response;
    response.set_audio_focus_state(
        aap_protobuf::service::control::message::AudioFocusNotification::AUDIO_FOCUS_STATE_GAIN);
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

void JniSession::onNavigationFocusRequest(
    const aap_protobuf::service::control::message::NavFocusRequestNotification& /*request*/)
{
    LOGI("Navigation focus request — granting");
    aap_protobuf::service::control::message::NavFocusNotification response;
    response.set_focus_type(
        aap_protobuf::service::control::message::NavFocusNotification::NAV_FOCUS_PROJECTED);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    controlChannel_->sendNavigationFocusResponse(response, std::move(promise));
    controlChannel_->receive(shared_from_this());
}

void JniSession::onByeByeRequest(
    const aap_protobuf::service::control::message::ByeByeRequest& /*request*/)
{
    LOGI("ByeBye request — disconnecting");
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
    bool charging = notification.charging_status();

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
    bool active = request.voice_session_type() != 0;

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
    controlChannel_->receive(shared_from_this());
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
// IVideoMediaSinkServiceEventHandler — video from phone
// ============================================================================

void JniSession::onChannelOpenRequest(
    const aap_protobuf::service::control::message::ChannelOpenRequest& /*request*/)
{
    LOGI("Video channel open request");
    aap_protobuf::service::control::message::ChannelOpenResponse response;
    response.set_status(aap_protobuf::shared::STATUS_OK);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [this](const auto& e) { this->onChannelError(e); });
    videoChannel_->sendChannelOpenResponse(response, std::move(promise));
    videoChannel_->receive(shared_from_this());
}

void JniSession::onMediaChannelSetupRequest(
    const aap_protobuf::service::media::shared::message::Setup& request)
{
    LOGI("Video setup: type=%d", request.media_codec_type());

    aap_protobuf::service::media::shared::message::Config config;
    config.set_status(aap_protobuf::shared::STATUS_OK);
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
    focus.set_focus_mode(
        aap_protobuf::service::media::video::message::VideoFocusNotification::VIDEO_FOCUS_PROJECTED);
    focus.set_unrequested(false);
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
    // Hot path — video frame. Send ACK immediately (flow control).
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(0);
    ack.set_value(1);
    auto promise = aasdk::channel::SendPromise::defer(*strand_);
    promise->then([]() {}, [](const auto&) {});
    videoChannel_->sendMediaAckIndication(ack, std::move(promise));

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
                                static_cast<jint>(sdrConfig_.videoHeight));
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
// startAllHandlers() — begin receiving on all service channels
// ============================================================================

void JniSession::startAllHandlers()
{
    LOGI("Starting all service handlers");
    // Video is already receiving from onChannelOpenRequest path.
    // Start the remaining channels so they can receive when the phone opens them.
    // Note: Audio sink handlers need separate event handler classes because
    // JniSession already implements IVideoMediaSinkServiceEventHandler and
    // the audio interface has the same method names. For now, audio channels
    // are declared in the SDR and the phone will open them — they just need
    // their onChannelOpenRequest/onMediaWithTimestamp handlers.
    // This is handled via the channel's receive() mechanism.
}

// ============================================================================
// buildServiceDiscoveryResponse() — tell phone what we support
// ============================================================================

void JniSession::buildServiceDiscoveryResponse(
    aap_protobuf::service::control::message::ServiceDiscoveryResponse& response)
{
    response.set_head_unit_name("OpenAutoLink");
    response.set_car_model(sdrConfig_.vehicleModel);
    response.set_car_year(sdrConfig_.vehicleYear);
    response.set_car_serial("OAL-JNI-1");
    response.set_driver_position(
        sdrConfig_.driverPosition == 1
            ? aap_protobuf::shared::DRIVER_POSITION_RIGHT
            : aap_protobuf::shared::DRIVER_POSITION_LEFT);
    response.set_headunit_make(sdrConfig_.vehicleMake);
    response.set_headunit_model(sdrConfig_.vehicleModel);
    response.set_sw_build("1");
    response.set_sw_version("1.0");
    response.set_can_play_native_media_during_vr(false);
    response.set_hide_clock(false);

    // ---- Video channel ----
    auto* videoDesc = response.add_channels();
    auto* videoSink = videoDesc->mutable_media_sink_service();
    auto* videoConfig = videoSink->mutable_video_configs()->Add();

    auto res = aap_protobuf::shared::VIDEO_RESOLUTION_1080P;
    if (sdrConfig_.videoHeight <= 480) res = aap_protobuf::shared::VIDEO_RESOLUTION_480P;
    else if (sdrConfig_.videoHeight <= 720) res = aap_protobuf::shared::VIDEO_RESOLUTION_720P;
    else if (sdrConfig_.videoHeight <= 1080) res = aap_protobuf::shared::VIDEO_RESOLUTION_1080P;
    else if (sdrConfig_.videoHeight <= 1440) res = aap_protobuf::shared::VIDEO_RESOLUTION_1440P;
    else res = aap_protobuf::shared::VIDEO_RESOLUTION_2160P;
    videoConfig->set_video_resolution(res);

    auto fps = aap_protobuf::shared::VIDEO_FPS_60;
    if (sdrConfig_.videoFps <= 30) fps = aap_protobuf::shared::VIDEO_FPS_30;
    videoConfig->set_video_fps(fps);
    videoConfig->set_codec_type(aap_protobuf::shared::CODEC_TYPE_H264_BP);
    videoConfig->set_density(sdrConfig_.videoDpi);
    if (sdrConfig_.marginWidth > 0) videoConfig->set_margin_width(sdrConfig_.marginWidth);
    if (sdrConfig_.marginHeight > 0) videoConfig->set_margin_height(sdrConfig_.marginHeight);

    auto* touchConfig = videoConfig->mutable_touch_config();
    touchConfig->set_width(sdrConfig_.videoWidth);
    touchConfig->set_height(sdrConfig_.videoHeight);

    // H.265 + VP9 configs
    auto* vc265 = videoSink->mutable_video_configs()->Add();
    *vc265 = *videoConfig;
    vc265->set_codec_type(aap_protobuf::shared::CODEC_TYPE_H265);

    auto* vcVp9 = videoSink->mutable_video_configs()->Add();
    *vcVp9 = *videoConfig;
    vcVp9->set_codec_type(aap_protobuf::shared::CODEC_TYPE_VP9);

    videoSink->set_type(aap_protobuf::shared::SINK);
    videoSink->set_available_while_in_call(true);

    // ---- Media audio (48kHz stereo) ----
    auto* maDesc = response.add_channels();
    auto* maSink = maDesc->mutable_media_sink_service();
    auto* maCfg = maSink->mutable_audio_configs()->Add();
    maCfg->set_sample_rate(48000);
    maCfg->set_bit_depth(16);
    maCfg->set_channel_count(2);
    maSink->set_type(aap_protobuf::shared::SINK);
    maSink->set_available_while_in_call(true);

    // ---- Guidance audio (16kHz mono) ----
    auto* gaDesc = response.add_channels();
    auto* gaSink = gaDesc->mutable_media_sink_service();
    auto* gaCfg = gaSink->mutable_audio_configs()->Add();
    gaCfg->set_sample_rate(16000);
    gaCfg->set_bit_depth(16);
    gaCfg->set_channel_count(1);
    gaSink->set_type(aap_protobuf::shared::SINK);
    gaSink->set_available_while_in_call(true);

    // ---- System audio (16kHz mono) ----
    auto* saDesc = response.add_channels();
    auto* saSink = saDesc->mutable_media_sink_service();
    auto* saCfg = saSink->mutable_audio_configs()->Add();
    saCfg->set_sample_rate(16000);
    saCfg->set_bit_depth(16);
    saCfg->set_channel_count(1);
    saSink->set_type(aap_protobuf::shared::SINK);
    saSink->set_available_while_in_call(true);

    // ---- Mic input (16kHz mono) ----
    auto* micDesc = response.add_channels();
    auto* micSrc = micDesc->mutable_media_source_service();
    auto* micCfg = micSrc->mutable_audio_configs()->Add();
    micCfg->set_sample_rate(16000);
    micCfg->set_bit_depth(16);
    micCfg->set_channel_count(1);
    micSrc->set_type(aap_protobuf::shared::SOURCE);

    // ---- Sensor channel ----
    auto* senDesc = response.add_channels();
    auto* senSrc = senDesc->mutable_sensor_source_service();
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_LOCATION);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_COMPASS);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_SPEED);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_RPM);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_FUEL);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_GEAR);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_PARKING_BRAKE);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_NIGHT_DATA);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_DRIVING_STATUS);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_ENVIRONMENT);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_ACCEL);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_GYRO);
    senSrc->add_sensors()->set_type(aap_protobuf::shared::SENSOR_TYPE_GPS_SATELLITE);

    // ---- Input channel ----
    auto* inDesc = response.add_channels();
    auto* inSrc = inDesc->mutable_input_source_service();
    auto* inTouch = inSrc->mutable_touch_config();
    inTouch->set_width(sdrConfig_.videoWidth);
    inTouch->set_height(sdrConfig_.videoHeight);

    // ---- Bluetooth channel ----
    if (!sdrConfig_.btMac.empty()) {
        auto* btDesc = response.add_channels();
        auto* btSvc = btDesc->mutable_bluetooth_service();
        btSvc->set_adapter_address(sdrConfig_.btMac);
        btSvc->add_supported_pairing_methods(aap_protobuf::shared::BLUETOOTH_PAIRING_METHOD_HFP);
        btSvc->add_supported_pairing_methods(aap_protobuf::shared::BLUETOOTH_PAIRING_METHOD_A2DP);
    }

    // ---- Nav/media/phone status channels ----
    response.add_channels()->mutable_navigation_status_service();
    response.add_channels()->mutable_media_playback_status_service();
    response.add_channels()->mutable_phone_status_service();

    LOGI("SDR built: %d channels, %d bytes", response.channels_size(),
         static_cast<int>(response.ByteSizeLong()));
}

// ============================================================================
// Input forwarding (app → phone)
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
    if (!streaming_ || !micOpen_ || !micChannel_) return;
    std::vector<uint8_t> dataCopy(data, data + length);
    ioService_->post([this, dataCopy = std::move(dataCopy)]() {
        aasdk::common::Data audioData(dataCopy.begin(), dataCopy.end());
        auto ts = static_cast<aasdk::messenger::Timestamp::ValueType>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        micChannel_->sendMediaSourceWithTimestampIndication(ts, audioData, std::move(promise));
    });
}

void JniSession::requestKeyframe()
{
    if (!streaming_ || !videoChannel_) return;
    ioService_->post([this]() {
        LOGI("Requesting keyframe (VideoFocusIndication)");
        aap_protobuf::service::media::video::message::VideoFocusNotification focus;
        focus.set_focus_mode(
            aap_protobuf::service::media::video::message::VideoFocusNotification::VIDEO_FOCUS_PROJECTED);
        focus.set_unrequested(false);
        auto promise = aasdk::channel::SendPromise::defer(*strand_);
        promise->then([]() {}, [](const auto&) {});
        videoChannel_->sendVideoFocusIndication(focus, std::move(promise));
    });
}

// ============================================================================
// Typed vehicle sensor methods — each builds SensorBatch and sends
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

} // namespace openautolink::jni
