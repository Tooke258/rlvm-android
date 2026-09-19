#include "android/jni_saf_backend.h"

#include <android/log.h>

#include <string>
#include <vector>

#include "android/saf_file_system.h"

namespace rlvm_android {
namespace {

constexpr char kLogTag[] = "rlvm-native";

JavaVM* g_vm = nullptr;

// 从 native 回调 Kotlin。我们的调用都发生在 Java 线程上（Kotlin 的工作线程），
// 因此绝大多数情况下线程已挂接；仍处理需要临时挂接的情况。
class ScopedEnv {
 public:
  ScopedEnv() {
    if (g_vm == nullptr) return;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_OK) {
      return;
    }
    if (g_vm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
      attached_ = true;
    } else {
      env_ = nullptr;
    }
  }
  ~ScopedEnv() {
    if (attached_ && g_vm != nullptr) g_vm->DetachCurrentThread();
  }
  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

class JniSafBackend : public SafBackend {
 public:
  JniSafBackend(JNIEnv* env, jobject backend) : object_(env->NewGlobalRef(backend)) {
    jclass cls = env->GetObjectClass(backend);
    m_exists_ = env->GetMethodID(cls, "exists", "(Ljava/lang/String;)Z");
    m_is_directory_ = env->GetMethodID(cls, "isDirectory", "(Ljava/lang/String;)Z");
    m_size_ = env->GetMethodID(cls, "size", "(Ljava/lang/String;)J");
    m_list_ = env->GetMethodID(cls, "listDirectory", "(Ljava/lang/String;)[Ljava/lang/String;");
    m_open_fd_ = env->GetMethodID(cls, "openFd", "(Ljava/lang/String;)I");
  }

  ~JniSafBackend() override {
    ScopedEnv scoped;
    if (scoped.get() != nullptr && object_ != nullptr) {
      scoped.get()->DeleteGlobalRef(object_);
    }
  }

  bool IsUsable() const {
    return object_ != nullptr && m_exists_ != nullptr && m_is_directory_ != nullptr &&
           m_size_ != nullptr && m_list_ != nullptr && m_open_fd_ != nullptr;
  }

  bool Exists(const std::string& rel_path) override {
    ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) return false;
    jstring arg = env->NewStringUTF(rel_path.c_str());
    const jboolean result = env->CallBooleanMethod(object_, m_exists_, arg);
    env->DeleteLocalRef(arg);
    return result == JNI_TRUE;
  }

  bool IsDirectory(const std::string& rel_path) override {
    ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) return false;
    jstring arg = env->NewStringUTF(rel_path.c_str());
    const jboolean result = env->CallBooleanMethod(object_, m_is_directory_, arg);
    env->DeleteLocalRef(arg);
    return result == JNI_TRUE;
  }

  long long Size(const std::string& rel_path) override {
    ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) return -1;
    jstring arg = env->NewStringUTF(rel_path.c_str());
    const jlong result = env->CallLongMethod(object_, m_size_, arg);
    env->DeleteLocalRef(arg);
    return static_cast<long long>(result);
  }

  std::vector<std::string> ListDirectory(const std::string& rel_path) override {
    std::vector<std::string> names;
    ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) return names;

    jstring arg = env->NewStringUTF(rel_path.c_str());
    jobjectArray array = static_cast<jobjectArray>(
        env->CallObjectMethod(object_, m_list_, arg));
    env->DeleteLocalRef(arg);
    if (array == nullptr) return names;

    const jsize count = env->GetArrayLength(array);
    for (jsize i = 0; i < count; ++i) {
      jstring item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
      if (item == nullptr) continue;
      const char* chars = env->GetStringUTFChars(item, nullptr);
      if (chars != nullptr) {
        names.emplace_back(chars);
        env->ReleaseStringUTFChars(item, chars);
      }
      env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(array);
    return names;
  }

  int OpenFd(const std::string& rel_path) override {
    ScopedEnv scoped;
    JNIEnv* env = scoped.get();
    if (env == nullptr) return -1;
    jstring arg = env->NewStringUTF(rel_path.c_str());
    const jint fd = env->CallIntMethod(object_, m_open_fd_, arg);
    env->DeleteLocalRef(arg);
    return static_cast<int>(fd);
  }

 private:
  jobject object_;
  jmethodID m_exists_ = nullptr;
  jmethodID m_is_directory_ = nullptr;
  jmethodID m_size_ = nullptr;
  jmethodID m_list_ = nullptr;
  jmethodID m_open_fd_ = nullptr;
};

std::shared_ptr<JniSafBackend> g_jni_backend;

}  // namespace

void SetJavaVm(JavaVM* vm) { g_vm = vm; }

bool HasJniSafBackend() { return g_jni_backend != nullptr; }

void InstallJniSafBackend(JNIEnv* env, jobject backend) {
  std::shared_ptr<JniSafBackend> impl(new JniSafBackend(env, backend));
  if (!impl->IsUsable()) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "SafFileSystem backend is missing expected methods");
    return;
  }
  // 先设置门面再保留引用：SetSafBackend 会持有 shared_ptr。
  g_jni_backend = impl;
  SetSafBackend(impl);
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "SAF backend installed");
}

}  // namespace rlvm_android
