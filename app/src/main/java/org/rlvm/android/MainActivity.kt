package org.rlvm.android

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView

/**
 * T0.2 阶段的最小壳层：只负责证明「Kotlin -> JNI -> CMake/NDK -> 打包」整条链路可用。
 * 真正的 Compose + Material 3 界面在 T3.2 落地。
 */
class MainActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val probe = runCatching {
            "${NativeBridge.versionString()}\nnative ABI: ${NativeBridge.probeAbi()}-bit"
        }.getOrElse { error ->
            "native bridge unavailable: ${error.message}"
        }

        val label = TextView(this).apply {
            text = probe
            gravity = Gravity.CENTER
        }

        setContentView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER
                addView(label)
            }
        )
    }
}
