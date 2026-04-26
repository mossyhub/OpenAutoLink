/*
 * aasdk_jni.cpp — JNI entry point for aasdk-in-app.
 *
 * Exposes native methods to Kotlin's AasdkNative class.
 * Manages the JniSession lifecycle and dispatches calls.
 */
#include <jni.h>
#include <android/log.h>

#include <memory>
#include <mutex>

#include "jni_session.h"

#define LOG_TAG "OAL-AasdkJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM* gJvm = nullptr;
static std::mutex gSessionMutex;
static std::unique_ptr<openautolink::jni::JniSession> gSession;

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    gJvm = vm;
    LOGI("openautolink-jni loaded");
    return JNI_VERSION_1_6;
}

extern "C" {

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeCreateSession
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeCreateSession(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        LOGW("Session already exists, destroying old one");
        gSession->stop();
        gSession.reset();
    }
    gSession = std::make_unique<openautolink::jni::JniSession>(gJvm);
    LOGI("Native session created");
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeStartSession
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeStartSession(
    JNIEnv* env, jclass /*clazz*/,
    jobject transportPipe, jobject callback, jobject sdrConfig)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) {
        LOGE("No session — call createSession first");
        return;
    }
    gSession->start(env, transportPipe, callback, sdrConfig);
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeStopSession
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeStopSession(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        gSession->stop();
        gSession.reset();
    }
    LOGI("Native session stopped and destroyed");
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeSendTouchEvent
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeSendTouchEvent(
    JNIEnv* /*env*/, jclass /*clazz*/,
    jint action, jint pointerId, jfloat x, jfloat y, jint pointerCount)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        gSession->sendTouchEvent(action, pointerId, x, y, pointerCount);
    }
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeSendKeyEvent
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeSendKeyEvent(
    JNIEnv* /*env*/, jclass /*clazz*/,
    jint keyCode, jboolean isDown)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        gSession->sendKeyEvent(keyCode, isDown);
    }
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeSendGpsLocation
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeSendGpsLocation(
    JNIEnv* /*env*/, jclass /*clazz*/,
    jdouble lat, jdouble lon, jdouble alt,
    jfloat speed, jfloat bearing, jlong timestampMs)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        gSession->sendGpsLocation(lat, lon, alt, speed, bearing, timestampMs);
    }
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeSendVehicleSensor
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeSendVehicleSensor(
    JNIEnv* env, jclass /*clazz*/,
    jint sensorType, jbyteArray data)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;

    jsize len = env->GetArrayLength(data);
    auto* bytes = env->GetByteArrayElements(data, nullptr);
    gSession->sendVehicleSensor(sensorType,
        reinterpret_cast<const uint8_t*>(bytes), static_cast<size_t>(len));
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeSendMicAudio
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeSendMicAudio(
    JNIEnv* env, jclass /*clazz*/,
    jbyteArray data)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (!gSession) return;

    jsize len = env->GetArrayLength(data);
    auto* bytes = env->GetByteArrayElements(data, nullptr);
    gSession->sendMicAudio(
        reinterpret_cast<const uint8_t*>(bytes), static_cast<size_t>(len));
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeRequestKeyframe
 */
JNIEXPORT void JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeRequestKeyframe(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    if (gSession) {
        gSession->requestKeyframe();
    }
}

/*
 * Class:     com_openautolink_app_transport_aasdk_AasdkNative
 * Method:    nativeIsStreaming
 */
JNIEXPORT jboolean JNICALL
Java_com_openautolink_app_transport_aasdk_AasdkNative_nativeIsStreaming(
    JNIEnv* /*env*/, jclass /*clazz*/)
{
    std::lock_guard<std::mutex> lock(gSessionMutex);
    return gSession ? gSession->isStreaming() : JNI_FALSE;
}

} // extern "C"
