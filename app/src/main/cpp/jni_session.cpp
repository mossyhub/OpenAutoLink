/*
 * jni_session.cpp — aasdk session lifecycle + JNI callback dispatch.
 *
 * This is the core integration point: it creates the aasdk pipeline,
 * handles all AA channel events, and fires JNI callbacks to Kotlin.
 *
 * TODO: Wire up the full aasdk entity (HeadlessAutoEntity equivalent).
 *       This initial scaffold establishes the threading model and JNI
 *       callback pattern. The actual aasdk channel handler wiring will
 *       be added incrementally as we validate the NDK build.
 */
#include "jni_session.h"
#include "jni_transport.h"

#include <android/log.h>

// aasdk headers — these will compile once the NDK build is working
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Messenger/MessageInStream.hpp>
#include <aasdk/Messenger/MessageOutStream.hpp>
#include <aasdk/Messenger/Messenger.hpp>

#define LOG_TAG "OAL-JniSession"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace openautolink::jni {

JniSession::JniSession(JavaVM* jvm)
    : jvm_(jvm)
{
    ioService_ = std::make_unique<boost::asio::io_service>();
    ioWork_ = std::make_unique<boost::asio::io_service::work>(*ioService_);
    ioThread_ = std::thread(&JniSession::ioServiceThreadFunc, this);
    LOGI("JniSession created");
}

JniSession::~JniSession()
{
    stop();
    LOGI("JniSession destroyed");
}

void JniSession::start(JNIEnv* env, jobject transportPipe, jobject callback, jobject sdrConfig)
{
    if (streaming_) {
        LOGW("Session already streaming, ignoring start()");
        return;
    }

    // Create global refs for JNI objects that outlive this call
    callbackRef_ = env->NewGlobalRef(callback);
    jobject transportRef = env->NewGlobalRef(transportPipe);

    // Cache callback method IDs
    jclass cbClass = env->GetObjectClass(callback);
    cbMethods_.onSessionStarted = env->GetMethodID(cbClass, "onSessionStarted", "()V");
    cbMethods_.onSessionStopped = env->GetMethodID(cbClass, "onSessionStopped", "(Ljava/lang/String;)V");
    cbMethods_.onVideoFrame = env->GetMethodID(cbClass, "onVideoFrame", "([BJII)V");
    cbMethods_.onAudioFrame = env->GetMethodID(cbClass, "onAudioFrame", "([BIII)V");
    cbMethods_.onMicRequest = env->GetMethodID(cbClass, "onMicRequest", "(Z)V");
    cbMethods_.onNavigationStatus = env->GetMethodID(cbClass, "onNavigationStatus", "([B)V");
    cbMethods_.onNavigationTurn = env->GetMethodID(cbClass, "onNavigationTurn", "([B)V");
    cbMethods_.onNavigationDistance = env->GetMethodID(cbClass, "onNavigationDistance", "([B)V");
    cbMethods_.onMediaMetadata = env->GetMethodID(cbClass, "onMediaMetadata", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)V");
    cbMethods_.onMediaPlayback = env->GetMethodID(cbClass, "onMediaPlayback", "(IJ)V");
    cbMethods_.onPhoneStatus = env->GetMethodID(cbClass, "onPhoneStatus", "(II)V");
    cbMethods_.onPhoneBattery = env->GetMethodID(cbClass, "onPhoneBattery", "(IZ)V");
    cbMethods_.onVoiceSession = env->GetMethodID(cbClass, "onVoiceSession", "(Z)V");
    cbMethods_.onAudioFocusRequest = env->GetMethodID(cbClass, "onAudioFocusRequest", "(I)V");
    cbMethods_.onError = env->GetMethodID(cbClass, "onError", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cbClass);

    // Read SDR config from Kotlin
    jclass sdrClass = env->GetObjectClass(sdrConfig);
    jint videoWidth = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoWidth", "I"));
    jint videoHeight = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoHeight", "I"));
    jint videoFps = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoFps", "I"));
    jint videoDpi = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "videoDpi", "I"));
    jint marginWidth = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "marginWidth", "I"));
    jint marginHeight = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "marginHeight", "I"));
    jint pixelAspectE4 = env->GetIntField(sdrConfig, env->GetFieldID(sdrClass, "pixelAspectE4", "I"));
    env->DeleteLocalRef(sdrClass);

    LOGI("Starting session: video=%dx%d@%dfps dpi=%d margins=%d/%d pixelAspect=%d",
         videoWidth, videoHeight, videoFps, videoDpi, marginWidth, marginHeight, pixelAspectE4);

    // Create JNI transport from the Nearby stream pipe
    transport_ = std::make_shared<JniTransport>(*ioService_, jvm_, transportRef);

    // Build aasdk pipeline on the io_service thread
    ioService_->post([this, videoWidth, videoHeight, videoFps, videoDpi,
                      marginWidth, marginHeight, pixelAspectE4]() {
        /*
         * TODO: Build the full aasdk pipeline:
         *
         * 1. auto sslWrapper = std::make_shared<aasdk::transport::SSLWrapper>();
         * 2. auto cryptor = std::make_shared<aasdk::messenger::Cryptor>(sslWrapper);
         *    cryptor->init();
         * 3. auto inStream = std::make_shared<aasdk::messenger::MessageInStream>(
         *        *ioService_, transport_, cryptor);
         * 4. auto outStream = std::make_shared<aasdk::messenger::MessageOutStream>(
         *        *ioService_, transport_, cryptor);
         * 5. auto messenger = std::make_shared<aasdk::messenger::Messenger>(
         *        *ioService_, inStream, outStream);
         * 6. Create HeadlessAutoEntity (or equivalent) with all service handlers
         * 7. entity->start() → begins version exchange → handshake → SDR → streaming
         *
         * Service handlers fire JNI callbacks:
         * - onMediaWithTimestampIndication → cbMethods_.onVideoFrame
         * - onMediaWithTimestampIndication (audio) → cbMethods_.onAudioFrame
         * - onNavigationState → cbMethods_.onNavigationStatus
         * - etc.
         *
         * For now, mark session as streaming to validate the JNI pipeline.
         */
        streaming_ = true;
        LOGI("aasdk pipeline started (TODO: wire entity)");

        // Notify Kotlin
        JNIEnv* env = nullptr;
        bool attached = false;
        jint result = jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED) {
            jvm_->AttachCurrentThread(&env, nullptr);
            attached = true;
        }
        if (env && callbackRef_ && cbMethods_.onSessionStarted) {
            env->CallVoidMethod(callbackRef_, cbMethods_.onSessionStarted);
        }
        if (attached) jvm_->DetachCurrentThread();
    });
}

void JniSession::stop()
{
    if (stopped_.exchange(true)) return;

    LOGI("Stopping session");
    streaming_ = false;

    // Stop transport (closes streams, rejects promises)
    if (transport_) {
        transport_->stop();
    }

    // Stop io_service
    ioWork_.reset();
    ioService_->stop();

    if (ioThread_.joinable()) {
        ioThread_.join();
    }

    // Notify Kotlin of stop
    JNIEnv* env = nullptr;
    bool attached = false;
    jint result = jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        jvm_->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (env && callbackRef_ && cbMethods_.onSessionStopped) {
        jstring reason = env->NewStringUTF("stopped");
        env->CallVoidMethod(callbackRef_, cbMethods_.onSessionStopped, reason);
        env->DeleteLocalRef(reason);
    }

    // Release global refs
    if (env && callbackRef_) {
        env->DeleteGlobalRef(callbackRef_);
        callbackRef_ = nullptr;
    }
    if (attached) jvm_->DetachCurrentThread();

    transport_.reset();
}

void JniSession::sendTouchEvent(int action, int pointerId, float x, float y, int pointerCount)
{
    if (!streaming_) return;

    ioService_->post([this, action, pointerId, x, y, pointerCount]() {
        // TODO: Build InputReport protobuf and send via InputSourceService
        // entity->inputHandler()->sendTouchEvent(action, x, y);
    });
}

void JniSession::sendKeyEvent(int keyCode, bool isDown)
{
    if (!streaming_) return;

    ioService_->post([this, keyCode, isDown]() {
        // TODO: Build KeyEvent and send via InputSourceService
        // entity->inputHandler()->sendKeyEvent(keyCode, isDown, 0, false);
    });
}

void JniSession::sendGpsLocation(double lat, double lon, double alt,
                                  float speed, float bearing, long long timestampMs)
{
    if (!streaming_) return;

    ioService_->post([this, lat, lon, alt, speed, bearing, timestampMs]() {
        // TODO: Build SensorBatch with GPS and send via SensorSourceService
    });
}

void JniSession::sendVehicleSensor(int sensorType, const uint8_t* data, size_t length)
{
    if (!streaming_) return;

    std::vector<uint8_t> dataCopy(data, data + length);
    ioService_->post([this, sensorType, dataCopy = std::move(dataCopy)]() {
        // TODO: Build SensorBatch and send via SensorSourceService
    });
}

void JniSession::sendMicAudio(const uint8_t* data, size_t length)
{
    if (!streaming_) return;

    std::vector<uint8_t> dataCopy(data, data + length);
    ioService_->post([this, dataCopy = std::move(dataCopy)]() {
        // TODO: Send via MediaSourceService::sendMediaSourceWithTimestampIndication
    });
}

void JniSession::requestKeyframe()
{
    if (!streaming_) return;

    ioService_->post([this]() {
        // TODO: Send VideoFocusIndication via VideoServiceChannel
        LOGI("Keyframe requested");
    });
}

void JniSession::ioServiceThreadFunc()
{
    LOGI("io_service thread started");
    ioService_->run();
    LOGI("io_service thread exiting");
}

} // namespace openautolink::jni
