/*
 * jni_session.h — aasdk session wrapper for JNI.
 *
 * Creates the full aasdk pipeline (transport → cryptor → messenger → entity)
 * and dispatches AA events to Kotlin via JNI callbacks.
 */
#pragma once

#include <memory>
#include <thread>
#include <atomic>

#include <jni.h>
#include <boost/asio.hpp>

#include <aasdk/Transport/ITransport.hpp>
#include <aasdk/Messenger/ICryptor.hpp>
#include <aasdk/Messenger/IMessenger.hpp>

namespace openautolink::jni {

class JniTransport;

/**
 * Owns the aasdk session lifecycle:
 * 1. Kotlin provides a connected byte pipe (Nearby stream)
 * 2. JniSession wraps it in JniTransport → SSLWrapper → Cryptor → Messenger
 * 3. Creates service handlers that fire JNI callbacks for video/audio/nav/etc.
 * 4. Kotlin islands (VideoDecoder, AudioPlayer, InputManager) consume the callbacks
 *
 * Lifecycle:
 *   create() → start(transport_pipe) → [streaming] → stop() → destroy()
 */
class JniSession {
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

private:
    void ioServiceThreadFunc();

    JavaVM* jvm_;
    jobject callbackRef_ = nullptr;  // Global ref to Kotlin callback

    // Boost.Asio event loop
    std::unique_ptr<boost::asio::io_service> ioService_;
    std::unique_ptr<boost::asio::io_service::work> ioWork_;
    std::thread ioThread_;

    // aasdk pipeline (opaque — actual types depend on aasdk internals)
    std::shared_ptr<JniTransport> transport_;
    // Additional pipeline objects will be held here:
    // cryptor_, messenger_, entity_, service handlers
    // These are forward-declared to avoid pulling all aasdk headers into this header.

    std::atomic<bool> stopped_{false};
    std::atomic<bool> streaming_{false};

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
