// 用 JNI 把 Kotlin 侧的 SafFileSystem 接成 SafBackend 实现。

#ifndef RLVM_APP_SRC_MAIN_CPP_ANDROID_JNI_SAF_BACKEND_H_
#define RLVM_APP_SRC_MAIN_CPP_ANDROID_JNI_SAF_BACKEND_H_

#include <jni.h>

namespace rlvm_android {

// JNI_OnLoad 中保存 JavaVM，供 native 回调 Kotlin 时使用。
void SetJavaVm(JavaVM* vm);

// 把一个 Kotlin SafFileSystem 实例安装为当前后端（持有全局引用）。
void InstallJniSafBackend(JNIEnv* env, jobject backend);

// 是否已安装后端。
bool HasJniSafBackend();

}  // namespace rlvm_android

#endif  // RLVM_APP_SRC_MAIN_CPP_ANDROID_JNI_SAF_BACKEND_H_
