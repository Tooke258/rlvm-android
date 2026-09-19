// 顶层构建脚本：只声明插件版本，不在此处配置 Android。
// 版本组合为本机已验证过的 Gradle 9.1.0 + AGP 9.0.1（见 docs/DECISIONS.md D-003）。
// AGP 9 起内置 Kotlin 支持，不再需要单独声明 org.jetbrains.kotlin.android。
plugins {
    id("com.android.application") version "9.0.1" apply false
}
