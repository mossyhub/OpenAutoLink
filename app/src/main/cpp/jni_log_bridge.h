/*
 * jni_log_bridge.h — Route native log messages to Kotlin DiagnosticLog.
 *
 * Call oal_jni_log_init(env) from JNI_OnLoad to cache the method ref.
 * Then use OAL_LOGI / OAL_LOGW / OAL_LOGE macros — these write to both
 * Android logcat AND DiagnosticLog (ring buffer + file logger + TCP).
 *
 * Falls back silently to logcat-only if JNI init hasn't happened yet.
 */
#pragma once

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <cstdio>

namespace openautolink::jni {

// Cached JNI references — set once in JNI_OnLoad, read from any thread.
// JavaVM is used to attach non-JNI threads (boost::asio workers).
inline JavaVM* gLogJvm = nullptr;
inline jclass gDiagnosticLogClass = nullptr;   // global ref
inline jmethodID gNativeLogMethod = nullptr;

inline void oal_jni_log_init(JNIEnv* env, JavaVM* vm)
{
    gLogJvm = vm;
    jclass cls = env->FindClass("com/openautolink/app/diagnostics/DiagnosticLog");
    if (cls) {
        gDiagnosticLogClass = static_cast<jclass>(env->NewGlobalRef(cls));
        gNativeLogMethod = env->GetStaticMethodID(
            gDiagnosticLogClass, "nativeLog",
            "(ILjava/lang/String;Ljava/lang/String;)V");
        env->DeleteLocalRef(cls);
    }
}

// Forward a log line to Kotlin DiagnosticLog. Safe to call from any thread.
// level: ANDROID_LOG_DEBUG(3), ANDROID_LOG_INFO(4), ANDROID_LOG_WARN(5), ANDROID_LOG_ERROR(6)
inline void oal_jni_log(int level, const char* tag, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Always write to logcat
    __android_log_print(level, tag, "%s", buf);

    // Forward to Kotlin if JNI bridge is initialized
    if (!gLogJvm || !gDiagnosticLogClass || !gNativeLogMethod) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    jint result = gLogJvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        gLogJvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    if (!env) return;

    jstring jtag = env->NewStringUTF(tag);
    jstring jmsg = env->NewStringUTF(buf);
    if (jtag && jmsg) {
        env->CallStaticVoidMethod(gDiagnosticLogClass, gNativeLogMethod,
                                  static_cast<jint>(level), jtag, jmsg);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (jtag) env->DeleteLocalRef(jtag);
    if (jmsg) env->DeleteLocalRef(jmsg);

    if (attached) gLogJvm->DetachCurrentThread();
}

} // namespace openautolink::jni

// Convenience macros — drop-in replacement for LOGI/LOGW/LOGE.
// These write to BOTH logcat AND DiagnosticLog (file + TCP + ring buffer).
#define OAL_LOGI(tag, ...) openautolink::jni::oal_jni_log(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define OAL_LOGW(tag, ...) openautolink::jni::oal_jni_log(ANDROID_LOG_WARN, tag, __VA_ARGS__)
#define OAL_LOGE(tag, ...) openautolink::jni::oal_jni_log(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
