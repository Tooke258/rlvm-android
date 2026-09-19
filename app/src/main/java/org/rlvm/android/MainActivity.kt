package org.rlvm.android

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import kotlin.concurrent.thread

/**
 * T0.2 阶段的最小壳层：只负责证明「Kotlin -> JNI -> CMake/NDK -> 打包」整条链路可用。
 * 真正的 Compose + Material 3 界面在 T3.2 落地。
 */
class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val abi = runCatching {
            "${NativeBridge.versionString()}\nnative ABI: ${NativeBridge.probeAbi()}-bit"
        }.getOrElse { error ->
            "native bridge unavailable: ${error.message}"
        }

        val label = TextView(this).apply {
            text = abi
            gravity = Gravity.CENTER
            textSize = 12f
        }

        setContentView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                addView(label)
            }
        )

        // 引擎探针需要读文件，放到后台线程执行，结果回主线程更新。
        val probeDir = File(getExternalFilesDir(null), "probe")
        thread(name = "rlvm-probe") {
            val report = runCatching {
                buildString {
                    appendLine(NativeBridge.probeGameDir(probeDir.absolutePath))
                    appendLine("--- runScenario ---")
                    append(NativeBridge.runScenario(probeDir.absolutePath, 200000))
                }
            }.getOrElse { "probe failed: ${it.stackTraceToString()}" }
            runOnUiThread { label.text = "$abi\n\n$report" }
        }
    }
}
