plugins {
    id("com.android.application")
}

android {
    namespace = "org.rlvm.android"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.rlvm.android"
        minSdk = 31
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        // 发布目标只有这两个 ABI（task.md 硬性要求）。
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            // debug 额外编入 x86_64：本机模拟器是 x86_64 平台，而我们唯一的
            // Android 运行环境就是模拟器。发布构建（release）不含它，仍然只有
            // task.md 规定的 arm64-v8a 与 armeabi-v7a。
            ndk {
                abiFilters += "x86_64"
            }
        }
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
}
