package org.rlvm.android

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.opengl.GLSurfaceView
import android.view.Gravity
import android.view.MotionEvent
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import kotlin.concurrent.thread

/**
 * T2.2 阶段的最小壳层：
 *   「选择游戏目录」走 ACTION_OPEN_DOCUMENT_TREE 并持久化授权；
 *   「运行 SAF 引擎」通过 JNI 回调 Kotlin 读文件，全程不需要存储权限；
 *   「运行本机探针」保留应用专属目录的普通路径路径，便于不依赖用户操作的自动化迭代。
 *
 * 真正的 Compose + Material 3 界面在 T3.2 落地。
 */
class MainActivity : Activity() {

    private companion object {
        const val REQUEST_PICK_TREE = 1001
        const val PREFS = "rlvm"
        const val KEY_TREE_URI = "saf_tree_uri"
        // 引擎默认不限时运行（见 rlvm-diag.txt 的 time_budget_ms），
        // 指令上限也放宽到实际达不到的值，由「停止引擎」按钮负责收尾。
        const val MAX_INSTRUCTIONS = Int.MAX_VALUE

        // 触摸动作码，必须与 native-bridge.cpp 的 kTouch* 常量一致。
        const val TOUCH_DOWN = 0
        const val TOUCH_MOVE = 1
        const val TOUCH_UP = 2
    }

    private lateinit var output: TextView
    private lateinit var glView: GLSurfaceView
    private lateinit var renderer: RlvmRenderer

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 让 native 侧知道诊断文件（rlvm-diag.txt）放在哪里：应用的外部文件目录，
        // 可以用 adb push 直接改，不需要重新构建。
        getExternalFilesDir(null)?.let { dir ->
            runCatching { NativeBridge.setDiagnosticsDir(dir.absolutePath) }
        }

        output = TextView(this).apply {
            textSize = 11f
            setTextIsSelectable(true)
            setPadding(24, 24, 24, 24)
        }

        val pickButton = Button(this).apply {
            text = getString(R.string.pick_directory)
            setOnClickListener { pickDirectory() }
        }
        val safButton = Button(this).apply {
            text = getString(R.string.run_saf)
            setOnClickListener { runSafScenario() }
        }
        val pathButton = Button(this).apply {
            text = getString(R.string.run_app_dir)
            setOnClickListener { runAppDirScenario() }
        }
        // 引擎默认一直跑（停在标题/正文上，BGM 才会持续），所以需要一个喊停的入口。
        // requestStop 只置标志，native 在下一轮循环收尾；不必放到后台线程。
        val stopButton = Button(this).apply {
            text = getString(R.string.stop_engine)
            setOnClickListener {
                NativeBridge.requestStop()
                log("已请求停止引擎。")
            }
        }

        // 画面区：native 在引擎线程上合成帧，这里只负责显示。
        renderer = RlvmRenderer()
        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            // 触摸输入（T2.3）：把视图坐标换算成游戏帧坐标后交给引擎。
            // 在 renderer 里注入，引擎线程取走并广播给按钮对象。
            setOnTouchListener { _, event -> handleTouch(event) }
        }

        setContentView(
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                addView(
                    LinearLayout(this@MainActivity).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER
                        addView(pickButton)
                        addView(safButton)
                        addView(pathButton)
                        addView(stopButton)
                    }
                )
                addView(
                    glView,
                    LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, 0, 3f
                    )
                )
                addView(
                    ScrollView(this@MainActivity).apply {
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, 0, 2f
                        )
                        addView(output)
                    }
                )
            }
        )

        log(buildString {
            appendLine(runCatching {
                "${NativeBridge.versionString()} / native ABI ${NativeBridge.probeAbi()}-bit"
            }.getOrElse { "native bridge unavailable: ${it.message}" })
            val saved = savedTreeUri()
            append(if (saved == null) "尚未授权任何目录，请点“选择游戏目录”。" else "已授权目录：$saved")
        })
    }

    override fun onResume() {
        super.onResume()
        if (::glView.isInitialized) glView.onResume()
    }

    override fun onPause() {
        if (::glView.isInitialized) glView.onPause()
        super.onPause()
    }

    // -- SAF 目录授权 ------------------------------------------------------

    private fun pickDirectory() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
            )
        }
        @Suppress("DEPRECATION")
        startActivityForResult(intent, REQUEST_PICK_TREE)
    }

    @Deprecated("用 startActivityForResult 换取不引入 androidx 依赖（骨架刻意保持零第三方依赖）")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PICK_TREE) return

        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) {
            log("用户取消了目录选择。")
            return
        }

        // 持久化授权，应用重启后仍可访问（task.md T3.1 的要求）。
        val takeFlags = (data.flags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION))
        try {
            contentResolver.takePersistableUriPermission(uri, takeFlags)
        } catch (e: SecurityException) {
            log("无法持久化授权（provider 不支持或未授予）：${e.message}")
        }

        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putString(KEY_TREE_URI, uri.toString())
            .apply()

        log("已授权目录：$uri")
        runSafScenario()
    }

    private fun savedTreeUri(): Uri? =
        getSharedPreferences(PREFS, MODE_PRIVATE)
            .getString(KEY_TREE_URI, null)
            ?.let(Uri::parse)

    // -- 三种运行方式 ------------------------------------------------------

    /** 通过 SAF 运行：需要先授权目录。 */
    private fun runSafScenario() {
        val treeUri = savedTreeUri()
        if (treeUri == null) {
            log("请先点“选择游戏目录”完成授权。")
            return
        }
        log("--- SAF 运行 ---")
        background {
            val report = runCatching {
                NativeBridge.setSafBackend(SafFileSystem(applicationContext, treeUri))
                NativeBridge.runScenarioSaf(MAX_INSTRUCTIONS)
            }.getOrElse { "SAF run failed: ${it.stackTraceToString()}" }
            log(report)
        }
    }

    /** 走应用专属外部目录的普通路径，便于不依赖用户操作的自动化迭代。 */
    private fun runAppDirScenario() {
        val probeDir = File(getExternalFilesDir(null), "probe")
        log("--- 应用目录运行 (${probeDir.absolutePath}) ---")
        background {
            val report = runCatching {
                buildString {
                    appendLine(NativeBridge.probeGameDir(probeDir.absolutePath))
                    append(NativeBridge.runScenario(probeDir.absolutePath, MAX_INSTRUCTIONS))
                }
            }.getOrElse { "probe failed: ${it.stackTraceToString()}" }
            log(report)
        }
    }

    // -- 辅助 --------------------------------------------------------------

    private fun background(block: () -> Unit) {
        thread(name = "rlvm-task") { block() }
    }

    /**
     * 把触摸事件换算成游戏帧坐标后投递给引擎。
     *
     * 返回 false（落在黑边上）时事件交给系统，避免把黑边内的触摸也当成游戏输入。
     */
    private fun handleTouch(event: MotionEvent): Boolean {
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downTimeMs = System.currentTimeMillis()
                longPress = false
                TOUCH_DOWN
            }
            MotionEvent.ACTION_MOVE -> TOUCH_MOVE
            MotionEvent.ACTION_UP -> {
                longPress = System.currentTimeMillis() - downTimeMs >= 400
                TOUCH_UP
            }
            MotionEvent.ACTION_CANCEL -> TOUCH_UP
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> return false
            else -> return false
        }

        val point = renderer.mapToFrame(event.x, event.y) ?: return false
        // 长按 = 鼠标右键（用来打开游戏菜单/退出），短按 = 左键。
        // 长按 = 右键（打开游戏菜单/退出），短按 = 左键（推进文字）。
        val buttons = if (action == TOUCH_UP && longPress) 2 else 1
        runCatching { NativeBridge.touchEvent(action, point.x, point.y, buttons) }
        return true
    }

    private var longPress = false
    private var downTimeMs = 0L

    private fun log(text: String) {
        runOnUiThread {
            output.text = "${output.text}\n$text"
        }
    }
}
