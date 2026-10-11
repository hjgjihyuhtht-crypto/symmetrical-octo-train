// Ponte do jogo Expedição Ativa (raylib em NativeActivity) com o Kotlin.
// Kotlin: com.kinect3dstudio.nativebridge.NativeGame e com.kinect3dstudio.game.GameActivity.
//
// O Kotlin abre o Kinect (UsbManager + libfreenect, mesmo código da tela de sensores) e publica o
// handle aqui. O jogo lê por app/android_bridge.h. Antes de fechar o Kinect, o Kotlin chama
// nativeRevokeSource(), que espera o jogo soltar a fonte (acquire/release).
#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "app/android_bridge.h"
#include "jni_common.h"

extern "C" struct android_app *GetAndroidApp(void);  // raylib (rcore_android.c)

namespace k3d {
namespace android {

namespace {
std::mutex mu;
std::condition_variable cv;
SourceInfo info;
bool inUse = false;
bool retryRequested = false;

std::string jstr(JNIEnv *env, jstring s) {
    if (!s) return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
} // namespace

SourceInfo source() {
    std::lock_guard<std::mutex> lk(mu);
    return info;
}

bool acquire(uint64_t version) {
    std::lock_guard<std::mutex> lk(mu);
    if (info.version != version || info.state != SourceState::Ready || !info.src) return false;
    inUse = true;
    return true;
}

void release() {
    {
        std::lock_guard<std::mutex> lk(mu);
        inUse = false;
    }
    cv.notify_all();
}

void requestRetry() {
    std::lock_guard<std::mutex> lk(mu);
    retryRequested = true;
}

std::string internalDir() {
    struct android_app *app = GetAndroidApp();
    if (app && app->activity && app->activity->internalDataPath) return app->activity->internalDataPath;
    return "/data/local/tmp";
}

std::string externalDir() {
    struct android_app *app = GetAndroidApp();
    if (app && app->activity && app->activity->externalDataPath) return app->activity->externalDataPath;
    return internalDir();
}

} // namespace android
} // namespace k3d

using namespace k3d::android;

#define GAME_FN(name) Java_com_kinect3dstudio_nativebridge_NativeGame_##name

extern "C" {

JNIEXPORT void JNICALL GAME_FN(nativeSetWaiting)(JNIEnv *env, jobject, jstring message) {
    std::lock_guard<std::mutex> lk(mu);
    info.state = SourceState::Waiting;
    info.src = nullptr;
    info.message = jstr(env, message);
    ++info.version;
}

JNIEXPORT void JNICALL GAME_FN(nativeSetSource)(JNIEnv *env, jobject, jlong handle, jstring model) {
    std::lock_guard<std::mutex> lk(mu);
    info.state = handle ? SourceState::Ready : SourceState::Failed;
    info.src = k3d::fromHandle(handle);
    info.model = jstr(env, model);
    info.message.clear();
    ++info.version;
}

JNIEXPORT void JNICALL GAME_FN(nativeSetFailed)(JNIEnv *env, jobject, jstring message) {
    std::lock_guard<std::mutex> lk(mu);
    info.state = SourceState::Failed;
    info.src = nullptr;
    info.message = jstr(env, message);
    ++info.version;
}

JNIEXPORT void JNICALL GAME_FN(nativeSetSimulation)(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lk(mu);
    info.state = SourceState::Simulation;
    info.src = nullptr;
    info.message.clear();
    ++info.version;
}

// Retira a fonte do jogo e espera ele soltar (até timeoutMs). true se o jogo soltou.
JNIEXPORT jboolean JNICALL GAME_FN(nativeRevokeSource)(JNIEnv *env, jobject, jstring message, jint timeoutMs) {
    std::unique_lock<std::mutex> lk(mu);
    info.state = SourceState::Waiting;
    info.src = nullptr;
    info.message = jstr(env, message);
    ++info.version;
    const bool ok = cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return !inUse; });
    if (!ok) inUse = false;  // o jogo está parado (em segundo plano): ele confere a versão antes de ler de novo
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL GAME_FN(nativeTakeRetryRequest)(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lk(mu);
    const bool r = retryRequested;
    retryRequested = false;
    return r ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
