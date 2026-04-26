/*
 * jni_service_catalog.cpp — ServiceDiscoveryResponse builder for JNI mode.
 *
 * Equivalent to bridge's service_catalog.cpp but configured from Kotlin
 * SDR parameters passed through JNI.
 *
 * TODO: Port the SDR protobuf building logic from service_catalog.cpp.
 *       This will be wired into the entity's onServiceDiscoveryRequest handler.
 */
#include <android/log.h>

#define LOG_TAG "OAL-ServiceCatalog"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace openautolink::jni {

// Placeholder — will be populated when wiring the entity's control handler.
// The SDR includes: video config, audio configs (media/speech/system/input),
// sensor capabilities, BT MAC, touch config, display insets, vehicle identity.

} // namespace openautolink::jni
