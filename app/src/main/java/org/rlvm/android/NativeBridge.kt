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

    /**
     * 装配 AndroidSystem + RLMachine 并执行字节码，最多 [maxInstructions] 条指令。
     *
     * 返回可读报告（执行条数、停止原因、是否停机），异常同样写进报告。
     * 属于重 I/O + 计算操作，必须在后台线程调用。
     */
    external fun runScenario(gameDir: String, maxInstructions: Int): String

    /**
     * 安装 SAF 后端。之后 [runScenarioSaf] 就能通过用户授权的目录树访问文件，
     * 全程不需要任何存储权限。
     */
    external fun setSafBackend(backend: SafFileSystem)

    /**
     * 告知 native 侧诊断文件（rlvm-diag.txt）所在目录。
     *
     * 该文件用于在设备上调整诊断参数（逐条指令追踪、运行时长等），
     * 这样迭代时不必每次重新构建安装。
     */
    external fun setDiagnosticsDir(dir: String)

    /**
     * 请求停止当前正在运行的引擎。
     *
     * 引擎默认不限时运行（为了能一直停在标题/正文上），这个方法只置标志，
     * 引擎线程会在下一轮循环开头正常收尾并返回报告。
     */
    external fun requestStop()

    /**
     * 触摸输入。坐标必须是**游戏帧坐标**（0..帧宽 / 0..帧高），
     * 由 RlvmRenderer.mapToFrame 从视图坐标换算而来。
     *
     * action: 0=按下 1=移动 2=抬起。
     */
    external fun touchEvent(action: Int, x: Float, y: Float, buttons: Int)

    /**
     * 经由 SAF 装配并运行引擎（Gameexe.ini 整体读入，SEEN.TXT 走 fd + mmap）。
     * 返回可读报告。属于重 I/O + 计算操作，必须在后台线程调用。
     */
    external fun runScenarioSaf(maxInstructions: Int): String

    /** 当前呈现帧的尺寸：高 16 位为宽、低 16 位为高；暂无帧时返回 0。 */
    external fun getFrameSize(): Int

    /**
     * 把当前帧复制到 [buffer]（需为直接缓冲区，容量 >= 宽*高*4，RGBA8888）。
     * 返回帧序号；与上次相同表示没有新帧。失败返回 -1。
     */
    external fun copyFrameToBuffer(buffer: java.nio.ByteBuffer): Int
}
