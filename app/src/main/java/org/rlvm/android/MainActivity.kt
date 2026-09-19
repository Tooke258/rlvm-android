package org.rlvm.android

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
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
        const val MAX_INSTRUCTIONS = 200000
    }

    private lateinit var output: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

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
                    }
                )
                addView(
                    ScrollView(this@MainActivity).apply {
                        layoutParams = LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
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

    private fun log(text: String) {
        runOnUiThread {
            output.text = "${output.text}\n$text"
        }
    }
}
