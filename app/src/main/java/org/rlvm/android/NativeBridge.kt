package org.rlvm.android

/**
 * Kotlin 侧的原生桥接入口。
 *
 * 约束（task.md）：所有 native 方法必须由 JNI 侧使用 RegisterNatives 显式注册，
 * 不允许依赖按符号名动态查找。因此这里只声明 external 方法，
 * 真正的绑定发生在 native-bridge.cpp 的 JNI_OnLoad 中。
 */
object NativeBridge {

    /** 首次触达本对象时加载 librlvm.so，进而触发 JNI_OnLoad。 */
    init {
        System.loadLibrary("rlvm")
    }

    /** 返回 native 侧版本字符串，用于验证桥接链路是否打通（T1.4）。 */
    external fun versionString(): String

    /** 返回 native 侧指针位宽（64 / 32），用于验证 ABI 与预期一致。 */
    external fun probeAbi(): Int

    /**
     * 引擎探针：在 [gameDir] 下解析 Gameexe.ini 并打开 SEEN 归档，返回可读报告。
     *
     * 这是 Android 上第一次执行 RLVM 的真实逻辑（boost::filesystem + libreallive）。
     * 属于 I/O 操作，必须在后台线程调用。
     */
    external fun probeGameDir(gameDir: String): String
}
