package org.rlvm.android

import android.app.Activity
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.net.Uri
import android.os.Bundle
import android.graphics.drawable.GradientDrawable
import android.opengl.GLSurfaceView
import android.view.Gravity
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
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
        const val KEY_LANDSCAPE = "landscape"
        const val KEY_FIT_MODE = "fit_mode"
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
    // 悬浮球 + 侧边栏：游戏画面占满屏幕，控制项与日志收进侧栏，
    // 由右上角的小球（可拖动）展开/收起。
    private lateinit var panel: LinearLayout
    private lateinit var ball: TextView
    private var panelWidth = 0
    private var panelOpen = false

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
        // 横竖屏切换：选择会持久化，重启后保持。
        val orientationButton = Button(this).apply {
            text = getString(R.string.toggle_orientation)
            setOnClickListener { toggleOrientation() }
        }
        // 画面适配：循环切换（适配黑边 → 左右贴边 → 上下贴边 → 拉伸铺满）。
        val fitButton = Button(this).apply {
            text = getString(R.string.cycle_fit)
            setOnClickListener {
                val mode = renderer.cycleFitMode()
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putString(KEY_FIT_MODE, mode.name).apply()
                log("画面适配：${fitModeLabel(mode)}")
            }
        }

        // 画面区：native 在引擎线程上合成帧，这里只负责显示。
        renderer = RlvmRenderer()
        // 恢复上次选择：横竖屏 + 画面适配方式。
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        requestedOrientation = if (prefs.getBoolean(KEY_LANDSCAPE, false)) {
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        } else {
            ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
        renderer.fitMode = RlvmRenderer.FitMode.valueOf(
            prefs.getString(KEY_FIT_MODE, RlvmRenderer.FitMode.FIT.name)
                ?: RlvmRenderer.FitMode.FIT.name
        )
        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            // 触摸输入（T2.3）：把视图坐标换算成游戏帧坐标后交给引擎。
            // 在 renderer 里注入，引擎线程取走并广播给按钮对象。
            setOnTouchListener { _, event -> handleTouch(event) }
        }

        // ---- 悬浮球 + 侧边栏 -------------------------------------------------
        // 游戏画面应当占满屏幕；控制项与日志收进可滑出的侧栏，由小球开关。
        val density = resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()

        fun buttonRow(vararg views: android.view.View) = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            for (v in views) {
                addView(v, LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            }
        }

        panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xF0101010.toInt())
            setPadding(dp(8), dp(8), dp(8), dp(8))
            // 面板内也要有收起入口：球虽然始终在最上层（见下方添加顺序），
            // 但多一个明确的「收起」按钮更符合直觉。
            addView(Button(this@MainActivity).apply {
                text = "收起面板"
                setOnClickListener { togglePanel() }
            })
            addView(buttonRow(pickButton, safButton))
            addView(buttonRow(stopButton, orientationButton))
            addView(buttonRow(pathButton, fitButton))
            addView(
                ScrollView(this@MainActivity).apply { addView(output) },
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            )
        }

        // 悬浮球：可上下拖动（免得挡住游戏 UI），点击则展开/收起侧栏。
        var dragStartY = 0f
        var dragged = false
        ball = TextView(this).apply {
            text = "≡"
            textSize = 22f
            gravity = Gravity.CENTER
            setTextColor(0xFFFFFFFF.toInt())
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(0xCC202020.toInt())
                setStroke(dp(2), 0x88FFFFFF.toInt())
            }
            setOnTouchListener { v, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        dragStartY = event.rawY
                        dragged = false
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val dy = event.rawY - dragStartY
                        if (Math.abs(dy) > dp(6)) dragged = true
                        if (dragged) {
                            v.translationY += dy
                            dragStartY = event.rawY
                        }
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        if (!dragged) togglePanel()
                        true
                    }
                    else -> false
                }
            }
        }

        panelWidth = (resources.displayMetrics.widthPixels * 0.72f).toInt()
        val ballSize = dp(52)
        val root = FrameLayout(this)
        root.addView(glView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // 先加侧栏、后加球：FrameLayout 里后添加的在上层，
        // 否则面板一展开就把球盖住，用户再也点不到它（无法收起）。
        root.addView(panel, FrameLayout.LayoutParams(
            panelWidth, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END))
        val ballParams = FrameLayout.LayoutParams(
            ballSize, ballSize, Gravity.END or Gravity.CENTER_VERTICAL)
        ballParams.rightMargin = dp(6)
        root.addView(ball, ballParams)
        // 初始状态必须明确为「收起」。用 post 在首次布局后再设一次，
        // 以免个别机型在首次 layout 时把 translation 重置。
        panelOpen = false
        panel.translationX = panelWidth.toFloat()
        panel.post { if (!panelOpen) panel.translationX = panelWidth.toFloat() }
        setContentView(root)

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
        applySystemUi()
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

    /** 展开/收起右侧栏（滑入滑出，不重建任何视图）。 */
    private fun togglePanel() {
        panelOpen = !panelOpen
        panel.animate()
            .translationX(if (panelOpen) 0f else panelWidth.toFloat())
            .setDuration(180)
            .start()
    }

    /** 当前是否横屏（按实际配置判断，避免与持久化的期望值不一致）。 */
    private fun isLandscapeNow(): Boolean =
        resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE

    /**
     * 横竖屏切换并持久化。
     *
     * 注意用 LANDSCAPE / PORTRAIT 而不是 SENSOR：明确指定方向才能让"按钮切换"
     * 与系统自动旋转不打架。想要跟随重力感应时，把 UNSPECIFIED 作为第三种状态。
     */
    private fun toggleOrientation() {
        val toLandscape = !isLandscapeNow()
        requestedOrientation = if (toLandscape) {
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        } else {
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putBoolean(KEY_LANDSCAPE, toLandscape).apply()
        log(if (toLandscape) "已切到横屏（隐藏状态栏）。" else "已切到竖屏。")
    }

    /**
     * 横屏时隐藏系统状态栏（沉浸式），竖屏恢复。
     *
     * 用 BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE：从边缘下滑仍能临时唤出状态栏，
     * 避免用户被困在全屏里。
     */
    private fun applySystemUi() {
        val controller = window.insetsController ?: return
        if (isLandscapeNow()) {
            controller.hide(WindowInsets.Type.statusBars())
            controller.systemBarsBehavior =
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        } else {
            controller.show(WindowInsets.Type.statusBars())
        }
    }

    private fun fitModeLabel(mode: RlvmRenderer.FitMode): String = when (mode) {
        RlvmRenderer.FitMode.FIT -> "适配（黑边）"
        RlvmRenderer.FitMode.FILL_WIDTH -> "左右贴边"
        RlvmRenderer.FitMode.FILL_HEIGHT -> "上下贴边"
        RlvmRenderer.FitMode.STRETCH -> "拉伸铺满"
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // 横竖屏切换后重新应用沉浸式设置。
        applySystemUi()
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
