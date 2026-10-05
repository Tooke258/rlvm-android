package org.rlvm.android

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.content.res.Configuration
import android.view.KeyEvent
import android.net.Uri
import android.os.Bundle
import android.graphics.drawable.GradientDrawable
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
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
        // 文本容器：相对游戏目录树内的路径（不再用 document URI——系统文件选择器在
        // 部分机型上会崩，而且容器本来就应该放在游戏目录里）。
        const val KEY_CONTAINER_PATH = "text_container_path"
        const val KEY_CONTAINER_ON = "text_container_on"
        // 浮动按键栏（v0.2.1 T7.2）：默认关闭。
        const val KEY_INPUT_PAD_ON = "input_pad_on"
        const val KEY_LANDSCAPE = "landscape"
        const val KEY_FIT_MODE = "fit_mode"
        // 长按呼出右键的显式开关（v0.2.4 体验项）：默认沿用旧行为（开）。
        const val KEY_LONG_PRESS_RIGHT = "long_press_right"
        // 引擎默认不限时运行（见 rlvm-diag.txt 的 time_budget_ms），
        // 指令上限也放宽到实际达不到的值，由「停止引擎」按钮负责收尾。
        const val MAX_INSTRUCTIONS = Int.MAX_VALUE

        // 触摸动作码，必须与 native-bridge.cpp 的 kTouch* 常量一致。
        const val TOUCH_DOWN = 0
        const val TOUCH_MOVE = 1
        const val TOUCH_UP = 2

        // 上游 systems/base/event_listener.h 的 RLKEY_*（按键栏目前用得到的几个）。
        const val RLKEY_LSHIFT = 304
        const val RLKEY_LCTRL = 306

        // 引擎是否处于「挂起」（黑屏/后台）状态。
        //
        // 必须是**进程级**状态而不是 Activity 的字段：native 那台引擎活在进程里，
        // 而 Activity 可能被系统回收后重建（MIUI 上很常见）。若标志跟着 Activity 走，
        // 重建出来的实例以为"当前没挂起"，于是永远不去通知 native 恢复——引擎就
        // 一直卡在挂起里。放在 companion 里，谁读到都是同一份真相。
        @Volatile
        private var engineSuspended = false
    }

    private lateinit var output: TextView
    private lateinit var glView: GLSurfaceView
    private lateinit var renderer: RlvmRenderer
    // 悬浮球 + 侧边栏：游戏画面占满屏幕，控制项与日志收进侧栏，
    // 由右上角的小球（可拖动）展开/收起。
    private lateinit var panel: LinearLayout
    private lateinit var ball: TextView
    private lateinit var rootView: FrameLayout
    private var panelWidth = 0
    private var panelOpen = false
    // 「文本容器」开关按钮：标签要随状态变化，所以留一个引用。
    private lateinit var containerButton: Button
    // 浮动按键栏（v0.2.1 T7.2）：默认隐藏的输入层 + 它的开关按钮。
    private lateinit var inputOverlay: FrameLayout
    private lateinit var inputPadButton: Button
    // ---- 日志与「影片自测」开关（v0.2.3）----------------------------------
    // 面板里的日志区太小、而且重启就没了；现在日志同时落盘到
    // <外部文件目录>/rlvm-log.txt，启动时读回来，点「日志」是全屏可复制的视图。
    // native 侧（影片起播/结束等）也写同一个文件，见 android/app_log.h。
    private lateinit var logOverlay: LinearLayout
    private lateinit var logFile: File
    private lateinit var movTestButton: Button
    private var movTestEnabled = false
    // ---- v0.2.4 体验项 ------------------------------------------------------
    // 运行/停止一体化（引擎只有一个，不能并行）；长按呼出右键的显式开关。
    private lateinit var engineToggleButton: Button
    private lateinit var longPressButton: Button
    private var engineRunning = false
    private var longPressRightClick = true
    // 虚拟光标位置（游戏帧坐标）；由方向键推动，-1 表示还没初始化。
    private var cursorX = -1f
    private var cursorY = -1f
    private val padHandler = Handler(Looper.getMainLooper())
    private var padRepeat: Runnable? = null

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
        // 日志落盘：写 <外部文件目录>/rlvm-log.txt（native 侧也写这个文件），
        // 启动时把末尾读回面板，避免「重启就看不到刚才发生了什么」。
        logFile = File(getExternalFilesDir(null) ?: filesDir, "rlvm-log.txt")
        restoreLogFromFile()
        movTestEnabled = readDiagSwitch("mov_test").isNotEmpty()

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
        // 文本容器（语言）切换：汉化容器是另一个文件，用显式开关切换，不去覆盖游戏目录的
        // SEEN.TXT。开关状态与所选文件都持久化，引擎在「运行 SAF 引擎」时按它选归档。
        val pickContainerButton = Button(this).apply {
            text = getString(R.string.pick_text_container)
            setOnClickListener { pickTextContainer() }
        }
        containerButton = Button(this).apply {
            setOnClickListener { setTextContainerEnabled(!textContainerEnabled) }
        }
        updateContainerButtonLabel()
        // 浮动按键栏（v0.2.1 T7.2）：默认关。开启后显示方向键 + 加速 + 击打 + 右键。
        inputPadButton = Button(this).apply {
            setOnClickListener { setInputPadEnabled(!inputPadEnabled) }
        }
        updateInputPadButtonLabel()

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
        // ---- v0.2.4 体验项：按钮复用与显式开关 ---------------------------------
        // ① 运行/停止一体化：引擎只有一个、不能并行，合成一个按钮（复用 safButton）。
        engineToggleButton = safButton
        safButton.setOnClickListener { toggleEngine() }
        updateEngineButtonLabel()
        // ② 长按呼出右键做成显式开关（复用 stopButton 的位置）。
        longPressButton = stopButton
        longPressRightClick = prefs.getBoolean(KEY_LONG_PRESS_RIGHT, true)
        stopButton.setOnClickListener { toggleLongPressRightClick() }
        updateLongPressButtonLabel()

        val density = resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()
        // 球与侧栏的尺寸要在创建视图之前算好：球的拖动/贴边逻辑会用到它们。
        val ballSize = dp(52)
        panelWidth = (resources.displayMetrics.widthPixels * 0.72f).toInt()

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
            addView(buttonRow(pickContainerButton, containerButton))
            val logButton = Button(this@MainActivity).apply {
                text = "日志"
                setOnClickListener { showLogOverlay(true) }
            }
            addView(buttonRow(inputPadButton, logButton))
            movTestButton = Button(this@MainActivity).apply {
                setOnClickListener { toggleMovTest() }
            }
            updateMovTestButtonLabel()
            // 「导出渲染树」：抓一次性画面的图层结构（选项图标那类问题的判据）。
            val dumpTreeButton = Button(this@MainActivity).apply {
                text = "导出渲染树"
                setOnClickListener {
                    runCatching { NativeBridge.requestGraphicsDump() }
                    log("已请求导出渲染树（引擎线程导出后写入日志，点「日志」查看）")
                }
            }
            addView(buttonRow(movTestButton, dumpTreeButton))
        }

        // ---- 全屏日志层：面板里放不下日志，这里给它整屏 + 复制/清空 --------------
        logOverlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xF0000000.toInt())
            setPadding(dp(6), dp(6), dp(6), dp(6))
            addView(buttonRow(
                Button(this@MainActivity).apply {
                    text = "复制日志"
                    setOnClickListener { copyLogToClipboard() }
                },
                Button(this@MainActivity).apply {
                    text = "清空"
                    setOnClickListener { clearLog() }
                },
                Button(this@MainActivity).apply {
                    text = "关闭"
                    setOnClickListener { showLogOverlay(false) }
                }
            ))
            addView(
                ScrollView(this@MainActivity).apply { addView(output) },
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            )
            visibility = android.view.View.GONE
        }

        // ---- 浮动按键栏（v0.2.1 T7.2）：默认关闭的输入层 --------------------
        //
        // 给"方向 + 按住键 + 鼠标键"这类小游戏（LBEX 那种）补输入。四条通路：
        //   · 四向方向键 → 移动**鼠标光标**（RealLive 脚本今天唯一读得到的方向输入）；
        //   · 加速       → 按住时发键盘键（默认 LSHIFT）。脚本目前读不到键盘，这一路是
        //                  为将来补上小游戏 DLL 模拟预留的；
        //   · 击打       → 鼠标左键按下/抬起；
        //   · 右键       → 鼠标右键按下/抬起（与长按等价，显式按钮更方便）。
        // 整层默认 GONE；除按钮本身外不拦截触摸（未命中按钮时事件照旧落到游戏画面）。
        val padSize = dp(54)
        fun padButton(label: String,
                      onPress: () -> Unit,
                      onRelease: () -> Unit): TextView = TextView(this@MainActivity).apply {
            text = label
            textSize = 17f
            gravity = Gravity.CENTER
            setTextColor(0xFFFFFFFF.toInt())
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(0x55101010.toInt())
                setStroke(dp(1), 0x55FFFFFF.toInt())
            }
            setOnTouchListener { v, e ->
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        onPress()
                        v.isPressed = true
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        onRelease()
                        v.isPressed = false
                        true
                    }
                    else -> false
                }
            }
        }

        fun padRow(vararg cells: android.view.View?): LinearLayout {
            val row = LinearLayout(this@MainActivity)
            row.orientation = LinearLayout.HORIZONTAL
            for (cell in cells) {
                if (cell == null) {
                    row.addView(android.view.View(this@MainActivity),
                        LinearLayout.LayoutParams(padSize, padSize))
                } else {
                    row.addView(cell, LinearLayout.LayoutParams(padSize, padSize))
                }
            }
            return row
        }

        val dpad = LinearLayout(this@MainActivity).apply {
            orientation = LinearLayout.VERTICAL
            addView(padRow(null, padButton("\u2191", { startPadRepeat(0f, -8f) }, { stopPadRepeat() }), null))
            addView(padRow(
                padButton("\u2190", { startPadRepeat(-8f, 0f) }, { stopPadRepeat() }),
                null,
                padButton("\u2192", { startPadRepeat(8f, 0f) }, { stopPadRepeat() })
            ))
            addView(padRow(null, padButton("\u2193", { startPadRepeat(0f, 8f) }, { stopPadRepeat() }), null))
        }

        val actionPad = LinearLayout(this@MainActivity).apply {
            orientation = LinearLayout.VERTICAL
            addView(padButton("加速", { sendKey(RLKEY_LSHIFT, true) }, { sendKey(RLKEY_LSHIFT, false) }),
                LinearLayout.LayoutParams(padSize, padSize))
            addView(padButton("击打", { sendMouseButton(1, true) }, { sendMouseButton(1, false) }),
                LinearLayout.LayoutParams(padSize, padSize))
            addView(padButton("右键", { sendMouseButton(2, true) }, { sendMouseButton(2, false) }),
                LinearLayout.LayoutParams(padSize, padSize))
        }

        // 「Ctrl」按钮（v0.2.5）：按住 = 引擎的强制快进（ShouldFastForward 里 Ctrl
        // 那条路，原版"按住 Ctrl 跳过"就是这个）。「加」保持 Shift 不动——LB 的小游戏
        // 脚本读的是 Shift。
        actionPad.addView(
            padButton("Ctrl", { sendKey(RLKEY_LCTRL, true) },
                { sendKey(RLKEY_LCTRL, false) }),
            LinearLayout.LayoutParams(padSize, padSize))

        inputOverlay = FrameLayout(this@MainActivity).apply {
            addView(dpad, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.START or Gravity.BOTTOM).apply {
                leftMargin = dp(12)
                bottomMargin = dp(12)
            })
            addView(actionPad, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.END or Gravity.BOTTOM).apply {
                rightMargin = dp(12)
                bottomMargin = dp(12)
            })
            visibility = if (inputPadEnabled) android.view.View.VISIBLE else android.view.View.GONE
        }
        updateInputPadButtonLabel()

        // 悬浮球：可拖动（免得挡住游戏 UI），点击则展开/收起侧栏。
        // 拖动有两条规则：
        //   限位——球必须完整留在屏幕内，不允许被拖出去；
        //   贴边——松手后就近吸附到左/右边缘（悬浮球的标准交互）。
        var dragStartX = 0f
        var dragStartY = 0f
        var dragStartTx = 0f
        var dragStartTy = 0f
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
                val parentView = v.parent as? ViewGroup
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        dragStartX = event.rawX
                        dragStartY = event.rawY
                        dragStartTx = v.translationX
                        dragStartTy = v.translationY
                        dragged = false
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val dx = event.rawX - dragStartX
                        val dy = event.rawY - dragStartY
                        if (Math.abs(dx) > dp(6) || Math.abs(dy) > dp(6)) dragged = true
                        if (dragged) {
                            // 拖动范围按"球完整留在父容器内"计算：
                            // 水平方向从贴左到贴右，垂直方向以居中为基准上下对称。
                            val margin = dp(6)
                            val spanX = (parentView?.width ?: 0) - ballSize - 2 * margin
                            val spanY = ((parentView?.height ?: 0) / 2f) -
                                ballSize / 2f - margin
                            v.translationX = (dragStartTx + dx).coerceIn(-spanX.toFloat(), 0f)
                            v.translationY = (dragStartTy + dy)
                                .coerceIn(-spanY, spanY)
                        }
                        true
                    }
                    MotionEvent.ACTION_UP -> {
                        if (!dragged) {
                            togglePanel()
                        } else {
                            // 贴边：按球的中心落在左半还是右半，吸附到对应边缘。
                            val margin = dp(6)
                            val parentWidth = parentView?.width ?: 0
                            val baseLeft = parentWidth - ballSize - margin
                            val centerX = baseLeft + v.translationX + ballSize / 2f
                            val snapTx = if (centerX > parentWidth / 2f) {
                                0f
                            } else {
                                -(parentWidth - ballSize - 2 * margin).toFloat()
                            }
                            v.animate().translationX(snapTx).setDuration(150).start()
                        }
                        true
                    }
                    else -> false
                }
            }
        }

        val root = FrameLayout(this)
        rootView = root
        root.addView(glView, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // 按键栏压在游戏画面之上、控制面板之下（FrameLayout 里后加的在上层）。
        root.addView(inputOverlay, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // 先加侧栏、后加球：FrameLayout 里后添加的在上层，
        // 否则面板一展开就把球盖住，用户再也点不到它（无法收起）。
        root.addView(panel, FrameLayout.LayoutParams(
            panelWidth, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END))
        val ballParams = FrameLayout.LayoutParams(
            ballSize, ballSize, Gravity.END or Gravity.CENTER_VERTICAL)
        ballParams.rightMargin = dp(6)
        root.addView(ball, ballParams)
        // 日志层最后加 = 在最上层（需要时盖住面板与游戏）。
        root.addView(logOverlay, FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // 初始状态明确为「收起」：直接 GONE，连布局都不参与。
        panelOpen = false
        panel.visibility = android.view.View.GONE
        setContentView(root)

        log(buildString {
            appendLine(runCatching {
                "${NativeBridge.versionString()} / native ABI ${NativeBridge.probeAbi()}-bit"
            }.getOrElse { "native bridge unavailable: ${it.message}" })
            val saved = savedTreeUri()
            append(if (saved == null) "尚未授权任何目录，请点“选择游戏目录”。" else "已授权目录：$saved")
        })
        // 把持久化的文本容器开关推给 native（默认关 = 用游戏目录里的 Seen.txt）。
        applyTextContainer()
    }

    override fun onResume() {
        super.onResume()
        if (::glView.isInitialized) glView.onResume()
        applySystemUi()
    }

    // -- 黑屏 / 进后台 ⇄ 引擎挂起 -------------------------------------------
    // -- 浮动按键栏（v0.2.1 T7.2）--------------------------------------------
    //
    // 全部走已有的事件通道：方向键 = 移动鼠标光标；击打/右键 = 鼠标左右键；
    // 加速 = 键盘键（今天没有脚本级消费者，留给将来小游戏的 DLL 模拟）。

    private val inputPadEnabled: Boolean
        get() = getSharedPreferences(PREFS, MODE_PRIVATE)
            .getBoolean(KEY_INPUT_PAD_ON, false)

    private fun updateInputPadButtonLabel() {
        if (::inputPadButton.isInitialized) {
            inputPadButton.text = getString(
                if (inputPadEnabled) R.string.toggle_input_pad_on
                else R.string.toggle_input_pad_off)
        }
    }

    private fun setInputPadEnabled(enabled: Boolean) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putBoolean(KEY_INPUT_PAD_ON, enabled).apply()
        if (::inputOverlay.isInitialized) {
            inputOverlay.visibility =
                if (enabled) android.view.View.VISIBLE else android.view.View.GONE
        }
        updateInputPadButtonLabel()
        if (!enabled) stopPadRepeat()
        log(
            if (enabled) "按键栏：开（方向键移动光标 / 加速 / 击打 / 右键）"
            else "按键栏：关"
        )
    }

    /** 当前画面尺寸（宽, 高），来自 native 已呈现的帧。 */
    private fun frameSize(): Pair<Int, Int> {
        val packed = runCatching { NativeBridge.getFrameSize() }.getOrDefault(0)
        return (packed ushr 16).coerceAtLeast(1) to (packed and 0xFFFF).coerceAtLeast(1)
    }

    /** 虚拟光标：首次使用时从画面中心开始（RealLive 脚本读到的就是它的位置）。 */
    private fun ensureCursor() {
        if (cursorX >= 0f && cursorY >= 0f) return
        val (w, h) = frameSize()
        cursorX = w / 2f
        cursorY = h / 2f
    }

    /** 方向键按住：每 60ms 把虚拟光标推一格（游戏只认鼠标位置，不认方向键）。 */
    private fun startPadRepeat(dx: Float, dy: Float) {
        stopPadRepeat()
        val step = object : Runnable {
            override fun run() {
                ensureCursor()
                val (w, h) = frameSize()
                cursorX = (cursorX + dx).coerceIn(0f, (w - 1).toFloat())
                cursorY = (cursorY + dy).coerceIn(0f, (h - 1).toFloat())
                runCatching { NativeBridge.touchEvent(TOUCH_MOVE, cursorX, cursorY, 1) }
                padRepeat = this
                padHandler.postDelayed(this, 60)
            }
        }
        padRepeat = step
        step.run()
    }

    private fun stopPadRepeat() {
        padRepeat?.let { padHandler.removeCallbacks(it) }
        padRepeat = null
    }

    /** 鼠标键：mask 1=左键 2=右键。按下/抬起分别投递，脚本才能看到"按住"这个状态。 */
    private fun sendMouseButton(mask: Int, pressed: Boolean) {
        ensureCursor()
        runCatching {
            NativeBridge.touchEvent(
                if (pressed) TOUCH_DOWN else TOUCH_UP, cursorX, cursorY, mask)
        }
    }

    private fun sendKey(rlKeyCode: Int, pressed: Boolean) {
        runCatching { NativeBridge.keyEvent(rlKeyCode, pressed) }
    }

    //
    // 之前只有 onPause 停掉 GL 线程，引擎线程照样按 10ms 时间片推进字节码、音频也照样
    // 出声——表现就是「黑屏之后游戏还在跑」。现在屏幕熄灭或应用进后台就把引擎挂起
    // （不推进、不合成、不出声），回到前台原地继续。
    //
    // 两道保障：onStop/onStart 覆盖「应用被切走」，ACTION_SCREEN_OFF/ON 覆盖 MIUI 这类
    // 「Activity 仍然 started 但屏幕已熄灭」的厂商差异。
    private val screenReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            when (intent?.action) {
                Intent.ACTION_SCREEN_OFF -> setEngineSuspended(true)
                Intent.ACTION_SCREEN_ON -> setEngineSuspended(false)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        registerReceiver(screenReceiver, IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_SCREEN_ON)
        })
        setEngineSuspended(false)
    }

    override fun onStop() {
        setEngineSuspended(true)
        runCatching { unregisterReceiver(screenReceiver) }
        super.onStop()
    }

    /** 只在状态真的变化时过桥：onStop 与屏幕广播会重复报告同一件事。 */
    private fun setEngineSuspended(suspended: Boolean) {
        if (engineSuspended == suspended) return
        engineSuspended = suspended
        runCatching { NativeBridge.setEngineSuspended(suspended) }
            .onFailure { log("引擎挂起切换失败：${it.message}") }
    }

    override fun onPause() {
        if (::glView.isInitialized) glView.onPause()
        super.onPause()
    }

    // -- SAF 目录授权 ------------------------------------------------------

    // -- 文本容器（语言）开关 ----------------------------------------------
    //
    // 汉化容器是**另一个文件**（合并后的 Seen 归档），不能要求用户覆盖游戏目录里的
    // SEEN.TXT——那样就没法切回原版了。所以这里给一个显式开关 + 文件选择：
    // 开 = 引擎读用户指定的容器；关 = 读游戏目录里的 Seen.txt。
    // 注意：游戏目录里的 Seen####.txt 覆盖文件仍然优先于容器（补丁机制的既有语义）。

    /** 当前是否使用「指定文本容器」。 */
    private val textContainerEnabled: Boolean
        get() = getSharedPreferences(PREFS, MODE_PRIVATE)
            .getBoolean(KEY_CONTAINER_ON, false) && savedContainerPath() != null

    /** 所选容器在游戏目录里的相对路径（例如 "Seen-CN.TXT"）。 */
    private fun savedContainerPath(): String? =
        getSharedPreferences(PREFS, MODE_PRIVATE).getString(KEY_CONTAINER_PATH, null)

    private fun updateContainerButtonLabel() {
        if (::containerButton.isInitialized) {
            containerButton.text = getString(
                if (textContainerEnabled) R.string.toggle_text_container_on
                else R.string.toggle_text_container_off
            )
        }
    }

    /** 把当前开关状态推给 native：关 → 空串（用游戏目录 Seen.txt）。 */
    private fun applyTextContainer() {
        val path = if (textContainerEnabled) savedContainerPath() ?: "" else ""
        runCatching { NativeBridge.setTextContainerPath(path) }
            .onFailure { log("文本容器设置失败：${it.message}") }
        updateContainerButtonLabel()
    }

    private fun setTextContainerEnabled(enabled: Boolean) {
        if (enabled && savedContainerPath() == null) {
            log("还没有选择文本容器文件，先点「选择文本容器」。")
            return
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putBoolean(KEY_CONTAINER_ON, enabled).apply()
        applyTextContainer()
        log(
            if (enabled) "文本容器：开（${savedContainerPath()}）"
            else "文本容器：关（用游戏目录里的 Seen.txt）"
        )
    }

    /**
     * 选择文本容器：**不**调用系统文件选择器，直接在应用内列出游戏目录里的候选文件。
     *
     * 为什么不用 ACTION_OPEN_DOCUMENT：部分机型（本机 MIUI）在选择大文件时会把
     * 系统文件界面搞崩，而容器本来就应该和游戏数据放在一起。列目录走的是已经授权
     * 的 SAF 目录树（一次跨进程查询拿到全部条目），只有候选文件才去问一次大小。
     */
    private fun pickTextContainer() {
        val treeUri = savedTreeUri()
        if (treeUri == null) {
            log("请先点“选择游戏目录”完成授权，才能列出候选容器。")
            return
        }
        val saf = SafFileSystem(applicationContext, treeUri)
        val names = saf.listDirectory("")
            .filter { it.startsWith("F\t") }
            .map { it.substring(2) }
            .filter { isCandidateContainer(it) }
        if (names.isEmpty()) {
            log("游戏目录里没有找到候选容器（*.TXT / *.BIN / *.DAT / *.EXE / Seen*）。")
            return
        }
        val entries = names.map { it to saf.size(it) }.sortedByDescending { it.second }
        val labels = entries.map { (name, size) ->
            if (size > 0) "%s   (%.2f MB)".format(name, size / 1048576.0) else name
        }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("选择文本容器")
            .setItems(labels) { _, which ->
                val name = entries[which].first
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putString(KEY_CONTAINER_PATH, name)
                    .putBoolean(KEY_CONTAINER_ON, true)
                    .apply()
                applyTextContainer()
                log("已选择文本容器：$name（已开启）")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 候选容器的名字规则：RL 的文本归档通常叫 Seen*.txt，或藏在这些扩展名里。 */
    private fun isCandidateContainer(name: String): Boolean {
        val lower = name.lowercase()
        return lower.startsWith("seen") ||
            lower.endsWith(".txt") || lower.endsWith(".bin") ||
            lower.endsWith(".dat") || lower.endsWith(".exe")
    }

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
        // 每次开引擎都同步一次文本容器设置：开关可能刚被切过。
        applyTextContainer()
        engineRunning = true
        updateEngineButtonLabel()
        background {
            val report = runCatching {
                NativeBridge.setSafBackend(SafFileSystem(applicationContext, treeUri))
                NativeBridge.runScenarioSaf(MAX_INSTRUCTIONS)
            }.getOrElse { "SAF run failed: ${it.stackTraceToString()}" }
            log(report)
            runOnUiThread {
                engineRunning = false
                updateEngineButtonLabel()
            }
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
        // 这里**不做** translationX 滑动动画：GLSurfaceView 是 SurfaceView，
        // 占的是独立 surface 层，用变换动画把普通视图移到它上面时窗口不会重新
        // 计算那块区域的绘制——真机表现就是"第一次展开时上面一半不渲染，点一下
        // 才画出来"（已复现并截图确认）。
        // 用 VISIBLE/GONE 会让视图树重新布局并完整重绘，彻底避免残留区域。
        panel.visibility =
            if (panelOpen) android.view.View.VISIBLE else android.view.View.GONE
        rootView.requestLayout()
        rootView.invalidate()
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
    /** 硬件键盘转发（v0.2.5）：Android keycode → RLKEY_*（值取自
     *  systems/base/event_listener.h），走与触摸同一条 NativeBridge.keyEvent 通道。
     *  物理键盘/蓝牙键盘由此可用：Ctrl 按住 = 引擎的强制快进（原版 Ctrl 跳过），
     *  方向键 = 移动光标，Enter/Space = 推进，Esc = 菜单。 */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val rl = rlKeyOf(keyCode)
        if (rl == 0) return super.onKeyDown(keyCode, event)
        if (event.repeatCount > 0) return true  // 自动重复不重发
        sendKey(rl, true)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val rl = rlKeyOf(keyCode)
        if (rl == 0) return super.onKeyUp(keyCode, event)
        sendKey(rl, false)
        return true
    }

    /** Android KeyEvent → RLVM 的 RLKEY_*。注意 RLVM/SDL 的方向键顺序：
     *  上273 下274 右275 左276；Shift 304/303、Ctrl 306/305。 */
    private fun rlKeyOf(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_ESCAPE -> 27
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 13
        KeyEvent.KEYCODE_SPACE -> 32
        KeyEvent.KEYCODE_TAB -> 9
        KeyEvent.KEYCODE_DEL -> 8
        KeyEvent.KEYCODE_DPAD_UP -> 273
        KeyEvent.KEYCODE_DPAD_DOWN -> 274
        KeyEvent.KEYCODE_DPAD_RIGHT -> 275
        KeyEvent.KEYCODE_DPAD_LEFT -> 276
        KeyEvent.KEYCODE_CTRL_LEFT -> 306
        KeyEvent.KEYCODE_CTRL_RIGHT -> 305
        KeyEvent.KEYCODE_SHIFT_LEFT -> 304
        KeyEvent.KEYCODE_SHIFT_RIGHT -> 303
        else -> 0
    }

    private fun handleTouch(event: MotionEvent): Boolean {
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downTimeMs = System.currentTimeMillis()
                longPress = false
                TOUCH_DOWN
            }
            MotionEvent.ACTION_MOVE -> TOUCH_MOVE
            MotionEvent.ACTION_UP -> {
                // 长按=右键 只有在开关打开时生效（v0.2.4：改成显式开关）。
                longPress = longPressRightClick &&
                    System.currentTimeMillis() - downTimeMs >= 400
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

    // ---- v0.2.4 体验项：运行/停止一体化 + 长按右键开关 ------------------------

    /** 运行/停止按钮文案：引擎只有一个，不能并行，所以合成一个按钮。 */
    private fun updateEngineButtonLabel() {
        if (::engineToggleButton.isInitialized) {
            engineToggleButton.text = if (engineRunning) "停止引擎" else "运行引擎"
        }
    }

    private fun toggleEngine() {
        if (engineRunning) {
            NativeBridge.requestStop()
            log("已请求停止引擎（引擎线程会在循环尾部收尾）")
        } else {
            runSafScenario()
        }
    }

    private fun updateLongPressButtonLabel() {
        if (::longPressButton.isInitialized) {
            longPressButton.text = if (longPressRightClick) "长按右键：开" else "长按右键：关"
        }
    }

    /** 长按呼出右键的显式开关（持久化）。关掉后长按也只发左键，右键走按键栏。 */
    private fun toggleLongPressRightClick() {
        longPressRightClick = !longPressRightClick
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
            .putBoolean(KEY_LONG_PRESS_RIGHT, longPressRightClick).apply()
        updateLongPressButtonLabel()
        log(
            if (longPressRightClick) "长按=右键：开（短按仍是左键）"
            else "长按=右键：关（长按也只发左键；右键可用按键栏的「右键」按钮）"
        )
    }

    private fun log(text: String) {
        val stamp = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
            .format(java.util.Date())
        val line = "[$stamp] $text"
        runOnUiThread {
            output.text = "${output.text}\n$line"
        }
        runCatching { logFile.appendText("$line\n") }
    }

    /** 启动时把日志文件末尾读回面板（重启后还能看到刚才发生了什么）。 */
    private fun restoreLogFromFile() {
        val tail = runCatching {
            if (logFile.exists()) logFile.readText().takeLast(64 * 1024) else ""
        }.getOrDefault("")
        if (tail.isNotEmpty()) {
            output.text = "……（上次运行留下的日志）\n$tail"
        }
    }

    private fun showLogOverlay(show: Boolean) {
        logOverlay.visibility = if (show) android.view.View.VISIBLE else android.view.View.GONE
    }

    private fun copyLogToClipboard() {
        runCatching {
            val cm = getSystemService(Context.CLIPBOARD_SERVICE)
                as android.content.ClipboardManager
            cm.setPrimaryClip(android.content.ClipData.newPlainText("rlvm-log", output.text))
            "日志已复制到剪贴板（${output.text.length} 字）"
        }.onSuccess { log(it) }.onFailure { log("复制日志失败：${it.message}") }
    }

    private fun clearLog() {
        runCatching { logFile.writeText("") }
        output.text = ""
        log("日志已清空")
    }

    private fun updateMovTestButtonLabel() {
        movTestButton.text = if (movTestEnabled) "影片自测：开" else "影片自测：关"
    }

    /**
     * 影片自测开关（v0.2.3）：打开后写 `mov_test=OP00` 到诊断文件，下次点
     * 「运行 SAF 引擎」时引擎启动就自动播 MOV/OP00.mpg（.mpg 是游戏自己的影片）。
     * 目的：游戏自己的触发点在脚本 SEEN514（OP 场景），跑不到那里时也能验证上屏。
     */
    private fun toggleMovTest() {
        movTestEnabled = !movTestEnabled
        val ok = runCatching {
            writeDiagSwitch("mov_test", if (movTestEnabled) "OP00" else null)
            true
        }.getOrDefault(false)
        updateMovTestButtonLabel()
        when {
            !ok -> log("影片自测开关写入失败（诊断文件不可写？）")
            movTestEnabled -> log("影片自测已开：下次「运行 SAF 引擎」启动时会自动播 MOV/OP00.mpg")
            else -> log("影片自测已关：影片只在游戏脚本调用 movPlayEx 时播（SEEN514）")
        }
    }

    /** 读诊断开关（rlvm-diag.txt）的值；不存在返回空串。 */
    private fun readDiagSwitch(key: String): String {
        val dir = getExternalFilesDir(null) ?: return ""
        return runCatching {
            File(dir, "rlvm-diag.txt").readLines()
                .firstOrNull { it.startsWith("$key=") }
                ?.substringAfter("=")?.trim().orEmpty()
        }.getOrDefault("")
    }

    /** 改诊断开关：只动这一行，其它行原样保留；value = null 表示删掉这一行。 */
    private fun writeDiagSwitch(key: String, value: String?) {
        val dir = getExternalFilesDir(null) ?: return
        val f = File(dir, "rlvm-diag.txt")
        val lines = if (f.exists()) f.readLines().toMutableList() else mutableListOf()
        lines.removeAll { it.startsWith("$key=") }
        if (value != null) lines.add("$key=$value")
        f.writeText(lines.joinToString("\n") + "\n")
    }
}
