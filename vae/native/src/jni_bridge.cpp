#include <jni.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "arift_core_api.h"
#include "arift_log.h"
#include "arift_utils.h"
#include "memory_scanner.h"

using namespace arift;

namespace {

JNIEnv* g_env = nullptr;
JavaVM* g_vm = nullptr;

jstring toJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    g_env = env;
    ARIFT_INFO(kTagBridge, "JNI_OnLoad complete");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    (void)vm;
    (void)reserved;
    g_vm = nullptr;
    g_env = nullptr;
}

// ---------------------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeVersion(JNIEnv* env, jclass) {
    return toJString(env, AriftCore::version());
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeInit(JNIEnv* env, jclass,
                                                     jstring configPath) {
    const char* path = env->GetStringUTFChars(configPath, nullptr);
    int rc = AriftCore::instance().init(path ? path : "/data/data/com.arift.injector/files/arift");
    if (path) env->ReleaseStringUTFChars(configPath, path);
    return rc;
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    return AriftCore::instance().shutdown();
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeStatus(JNIEnv*, jclass) {
    return AriftCore::instance().status();
}

// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeAttach(JNIEnv*, jclass,
                                                       jint pid, jlong libBase) {
    return AriftCore::instance().attach(pid,
                                        static_cast<uintptr_t>(libBase));
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeDetach(JNIEnv*, jclass) {
    return AriftCore::instance().detach();
}

JNIEXPORT jboolean JNICALL
Java_com_arift_injector_core_NativeBridge_nativeIsAttached(JNIEnv*, jclass) {
    return AriftCore::instance().isAttached() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeTargetPid(JNIEnv*, jclass) {
    return AriftCore::instance().targetPid();
}

// ---------------------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeProbeMemory(JNIEnv* env, jclass,
                                                            jint pid,
                                                            jlong libBase) {
    ProcessMemory mem;
    if (!mem.open(pid)) {
        std::string out = "open failed errno=" + std::to_string(mem.lastErrno()) +
                          " (" + strerror(mem.lastErrno()) + ")";
        return toJString(env, out);
    }
    uint32_t magic = 0;
    if (!mem.read32(static_cast<uintptr_t>(libBase), magic)) {
        std::string out = "read failed errno=" + std::to_string(mem.lastErrno()) +
                          " (" + strerror(mem.lastErrno()) + ")";
        return toJString(env, out);
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "ok ro=%s base=0x%llx probe=0x%08x",
             mem.isReadOnly() ? "yes" : "no",
             static_cast<unsigned long long>(libBase), magic);
    return toJString(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRootInfo(JNIEnv* env, jclass) {
    std::string out = "root=";
    out += utils::isRootAvailable() ? "yes" : "no";
    FILE* p = popen("cat /sys/fs/selinux/enforce 2>/dev/null", "r");
    if (p) {
        char c = 0;
        if (fread(&c, 1, 1, p) == 1) {
            out += " selinux=";
            out += (c == '1') ? "enforcing" : "permissive";
        }
        pclose(p);
    }
    return toJString(env, out);
}

// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeSetFeature(JNIEnv*, jclass,
                                                           jint feature,
                                                           jboolean enabled) {
    return AriftCore::instance().setFeature(feature, enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_arift_injector_core_NativeBridge_nativeIsFeatureEnabled(JNIEnv*, jclass,
                                                                 jint feature) {
    return AriftCore::instance().isFeatureEnabled(feature) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_arift_injector_core_NativeBridge_nativeFeaturesMask(JNIEnv*, jclass) {
    return static_cast<jlong>(AriftCore::instance().featuresMask());
}

// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetRenderMode(JNIEnv*, jclass,
                                                                 jint mode) {
    return AriftCore::instance().espSetRenderMode(mode);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetBoxes(JNIEnv*, jclass,
                                                            jboolean v) {
    return AriftCore::instance().espSetBoxes(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetHealthBars(JNIEnv*, jclass,
                                                                 jboolean v) {
    return AriftCore::instance().espSetHealthBars(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetNames(JNIEnv*, jclass,
                                                            jboolean v) {
    return AriftCore::instance().espSetNames(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetCooldowns(JNIEnv*, jclass,
                                                                jboolean v) {
    return AriftCore::instance().espSetCooldowns(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetObjectives(JNIEnv*, jclass,
                                                                 jboolean v) {
    return AriftCore::instance().espSetObjectives(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspSetDistance(JNIEnv*, jclass,
                                                               jboolean v) {
    return AriftCore::instance().espSetDistance(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeEspRefreshNow(JNIEnv*, jclass) {
    return 0;
}

// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeMapHackSetFogBypass(JNIEnv*, jclass,
                                                                    jboolean v) {
    return AriftCore::instance().mapHackSetFogBypass(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeMapHackSetMinimapOverride(
    JNIEnv*, jclass, jboolean v) {
    return AriftCore::instance().mapHackSetMinimapOverride(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeMapHackSetVisionRadius(
    JNIEnv*, jclass, jfloat r) {
    return AriftCore::instance().mapHackSetVisionRadius(r);
}

// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRbSetEnabled(JNIEnv*, jclass,
                                                             jboolean v) {
    return AriftCore::instance().rbSetEnabled(v == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRbSetAggression(JNIEnv*, jclass,
                                                                jint level) {
    return AriftCore::instance().rbSetAggression(level);
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRbSetTargetRank(JNIEnv*, jclass,
                                                                jint rank) {
    return AriftCore::instance().rbSetTargetRank(rank);
}

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRbSnapshot(JNIEnv* env, jclass) {
    return toJString(env, AriftCore::instance().rbSnapshot());
}

JNIEXPORT jint JNICALL
Java_com_arift_injector_core_NativeBridge_nativeRbPump(JNIEnv*, jclass) {
    return AriftCore::instance().rbPump();
}

// ---------------------------------------------------------------------------

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeLastError(JNIEnv* env, jclass) {
    return toJString(env, AriftCore::instance().lastError());
}

JNIEXPORT jstring JNICALL
Java_com_arift_injector_core_NativeBridge_nativeDiagDump(JNIEnv* env, jclass) {
    return toJString(env, AriftCore::instance().diagDump());
}

}  // extern "C"