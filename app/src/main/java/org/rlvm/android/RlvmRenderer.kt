package org.rlvm.android

import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.graphics.PointF
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * 把 native 合成好的帧显示出来。
 *
 * 设计：引擎在自己的线程上把 DC0、图形对象与文字**在 CPU 上合成**成一张 RGBA 表面
 * （见 AndroidGraphicsSystem::frame_buffer_），GL 线程只负责把它传成纹理并铺满屏幕。
 * 这是「Surface 用 CPU 像素实现」这个取舍的自然结果——上游那套 GL 着色器管线
 * （多纹理混合、色调曲线、色彩遮罩）留到 T4.2 再逐项替换。
 *
 * 当前的近似之处：四角不透明度取平均而不是双线性插值，色彩遮罩与对象调制
 * 只做整块近似。这些都需要 GL 管线才能精确，已记录在 docs/COMPATIBILITY.md。
 */
class RlvmRenderer : GLSurfaceView.Renderer {

    private var program = 0
    private var texture = 0
    private var vertexBuffer = 0
    private var positionHandle = 0
    private var texCoordHandle = 0
    private var samplerHandle = 0

    private var vertices: FloatBuffer = ByteBuffer
        .allocateDirect(16 * 4)
        .order(ByteOrder.nativeOrder())
        .asFloatBuffer()

    private var pixels: ByteBuffer? = null
    private var frameWidth = 0
    private var frameHeight = 0
    private var uploadCount = 0

    /**
     * 画面适配方式。用户可在界面上循环切换，选择会持久化。
     *
     * 四边形的顶点是 NDC（-1..1）乘上 (scaleX, scaleY)：scale=1 表示铺满该方向。
     * 因此：
     *   FIT         —— 完整显示、短边留黑（默认，即原来的行为）
     *   FILL_WIDTH  —— 左右贴边（铺满宽度，纵向超出部分被裁掉或留黑）
     *   FILL_HEIGHT —— 上下贴边（铺满高度，横向超出部分被裁掉或留黑）
     *   STRETCH     —— 直接拉伸铺满（不保持宽高比）
     */
    enum class FitMode { FIT, FILL_WIDTH, FILL_HEIGHT, STRETCH }

    // 会被 UI 线程写入、GL 线程读取。
    @Volatile
    var fitMode: FitMode = FitMode.FIT

    fun cycleFitMode(): FitMode {
        fitMode = when (fitMode) {
            FitMode.FIT -> FitMode.FILL_WIDTH
            FitMode.FILL_WIDTH -> FitMode.FILL_HEIGHT
            FitMode.FILL_HEIGHT -> FitMode.STRETCH
            FitMode.STRETCH -> FitMode.FIT
        }
        return fitMode
    }
    // 这两个会被 UI 线程读取（触摸坐标换算），GL 线程写入，用 @Volatile 保证可见性。
    @Volatile
    private var surfaceWidth = 1
    @Volatile
    private var surfaceHeight = 1

    /**
     * 帧在视图里的缩放系数（居中显示，保持宽高比）。
     *
     * onDrawFrame 与 mapToFrame 必须用同一套规则，否则触摸坐标会和画面对不上——
     * 所以这里只留一份实现。
     */
    private fun fitScale(): Pair<Float, Float> {
        val frameAspect = frameWidth.toFloat() / frameHeight.toFloat()
        val surfaceAspect = surfaceWidth.toFloat() / surfaceHeight.toFloat()
        return when (fitMode) {
            FitMode.STRETCH -> Pair(1f, 1f)
            // 铺满宽度：左右贴边，纵向按比例放大（超出部分被视口裁掉）。
            FitMode.FILL_WIDTH -> Pair(1f, frameAspect / surfaceAspect)
            // 铺满高度：上下贴边。
            FitMode.FILL_HEIGHT -> Pair(surfaceAspect / frameAspect, 1f)
            FitMode.FIT -> if (surfaceAspect > frameAspect) {
                // 视图比画面宽：左右留黑边，画面占满高度。
                Pair(frameAspect / surfaceAspect, 1f)
            } else {
                // 视图比画面高：上下留黑边，画面占满宽度。
                Pair(1f, surfaceAspect / frameAspect)
            }
        }
    }

    /**
     * 把视图坐标（触摸点）换算成游戏帧坐标；落在黑边上时返回 null。
     *
     * 从 UI 线程调用。
     */
    fun mapToFrame(viewX: Float, viewY: Float): PointF? {
        if (frameWidth <= 0 || frameHeight <= 0) return null
        val (scaleX, scaleY) = fitScale()
        val width = surfaceWidth * scaleX
        val height = surfaceHeight * scaleY
        val left = (surfaceWidth - width) / 2f
        val top = (surfaceHeight - height) / 2f
        if (viewX < left || viewX > left + width || viewY < top || viewY > top + height) {
            return null
        }
        return PointF(
            (viewX - left) / width * frameWidth,
            (viewY - top) / height * frameHeight
        )
    }
    private var lastSerial = -1

    override fun onSurfaceCreated(unused: GL10?, config: EGLConfig?) {
        program = buildProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        positionHandle = GLES30.glGetAttribLocation(program, "aPosition")
        texCoordHandle = GLES30.glGetAttribLocation(program, "aTexCoord")
        samplerHandle = GLES30.glGetUniformLocation(program, "uTexture")

        val textures = IntArray(1)
        GLES30.glGenTextures(1, textures, 0)
        texture = textures[0]
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, texture)
        // 画面尺寸通常不是 2 的幂，必须用 CLAMP + LINEAR，否则会被当作 mipmap 不完整。
        GLES30.glTexParameteri(
            GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR
        )
        GLES30.glTexParameteri(
            GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR
        )
        GLES30.glTexParameteri(
            GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE
        )
        GLES30.glTexParameteri(
            GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE
        )

        val buffers = IntArray(1)
        GLES30.glGenBuffers(1, buffers, 0)
        vertexBuffer = buffers[0]
        Log.i(TAG, "surface created program=$program err=${GLES30.glGetError()}")
    }

    override fun onSurfaceChanged(unused: GL10?, width: Int, height: Int) {
        GLES30.glViewport(0, 0, width, height)
        surfaceWidth = maxOf(1, width)
        surfaceHeight = maxOf(1, height)
        Log.i(TAG, "surface changed ${width}x$height")
    }

    override fun onDrawFrame(unused: GL10?) {
        GLES30.glClearColor(0f, 0f, 0f, 1f)
        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT)

        // 每帧都要检查：GL 线程启动时引擎可能还没有产出任何帧，
        // 而帧是在引擎线程上陆续产生的。函数内部会判断有无新帧。
        uploadFrameIfChanged()
        if (frameWidth == 0 || frameHeight == 0) return

        GLES30.glUseProgram(program)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, texture)
        GLES30.glUniform1i(samplerHandle, 0)

        // 按画面比例做信箱式留边，避免 4:3 的画面被拉伸。
        val (scaleX, scaleY) = fitScale()

        // 帧缓冲第 0 行是画面顶部，而 GL 纹理原点在左下角，因此 V 轴要翻转。
        vertices.clear()
        vertices.put(
            floatArrayOf(
                -scaleX, -scaleY, 0f, 1f,
                scaleX, -scaleY, 1f, 1f,
                -scaleX, scaleY, 0f, 0f,
                scaleX, scaleY, 1f, 0f
            )
        )
        vertices.position(0)

        GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, vertexBuffer)
        GLES30.glBufferData(
            GLES30.GL_ARRAY_BUFFER, 16 * 4, vertices, GLES30.GL_DYNAMIC_DRAW
        )
        val stride = 4 * 4
        GLES30.glEnableVertexAttribArray(positionHandle)
        GLES30.glVertexAttribPointer(positionHandle, 2, GLES30.GL_FLOAT, false, stride, 0)
        GLES30.glEnableVertexAttribArray(texCoordHandle)
        GLES30.glVertexAttribPointer(texCoordHandle, 2, GLES30.GL_FLOAT, false, stride, 8)

        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)
    }

    /** 如果 native 侧有新帧就重新分配缓冲并上传纹理。 */
    private fun uploadFrameIfChanged() {
        val packed = NativeBridge.getFrameSize()
        if (packed == 0) return
        val width = packed ushr 16
        val height = packed and 0xFFFF
        if (width <= 0 || height <= 0) return

        if (width != frameWidth || height != frameHeight) {
            frameWidth = width
            frameHeight = height
            pixels = ByteBuffer.allocateDirect(width * height * 4)
                .order(ByteOrder.nativeOrder())
            lastSerial = -1
        }

        val buffer = pixels ?: return
        val serial = NativeBridge.copyFrameToBuffer(buffer)
        if (serial < 0 || serial == lastSerial) return
        lastSerial = serial

        buffer.position(0)
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, texture)
        GLES30.glTexImage2D(
            GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA, frameWidth, frameHeight, 0,
            GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, buffer
        )
        // 逐帧打日志会拖慢 GL 线程（logcat 是同步 I/O），只在开头和偶尔抽样时打。
        uploadCount++
        if (uploadCount <= 3 || uploadCount % 300 == 0) {
            Log.i(TAG, "uploaded frame ${frameWidth}x$frameHeight serial=$serial " +
                "count=$uploadCount err=${GLES30.glGetError()}")
        }
    }

    private fun buildProgram(vertexSource: String, fragmentSource: String): Int {
        val vertex = compile(GLES30.GL_VERTEX_SHADER, vertexSource)
        val fragment = compile(GLES30.GL_FRAGMENT_SHADER, fragmentSource)
        val program = GLES30.glCreateProgram()
        GLES30.glAttachShader(program, vertex)
        GLES30.glAttachShader(program, fragment)
        GLES30.glLinkProgram(program)
        val status = IntArray(1)
        GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(TAG, "program link failed: ${GLES30.glGetProgramInfoLog(program)}")
        } else {
            Log.i(TAG, "program linked (id=$program)")
        }
        GLES30.glDeleteShader(vertex)
        GLES30.glDeleteShader(fragment)
        return program
    }

    private fun compile(type: Int, source: String): Int {
        val shader = GLES30.glCreateShader(type)
        GLES30.glShaderSource(shader, source)
        GLES30.glCompileShader(shader)
        val status = IntArray(1)
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, status, 0)
        if (status[0] == 0) {
            Log.e(
                TAG,
                "shader compile failed (type=$type): ${GLES30.glGetShaderInfoLog(shader)}"
            )
        }
        return shader
    }

    private companion object {
        const val TAG = "rlvm-gl"
        // 注意：必须用 trimIndent() 去掉原始字符串开头的换行。
        // GLSL 规范允许 #version 前有空白，但 Adreno 驱动会直接报
        // "P0005: #version must be on the first line"。
        val VERTEX_SHADER = """
            #version 300 es
            in vec2 aPosition;
            in vec2 aTexCoord;
            out vec2 vTexCoord;
            void main() {
                vTexCoord = aTexCoord;
                gl_Position = vec4(aPosition, 0.0, 1.0);
            }
        """.trimIndent()

        val FRAGMENT_SHADER = """
            #version 300 es
            precision mediump float;
            in vec2 vTexCoord;
            uniform sampler2D uTexture;
            out vec4 fragColor;
            void main() {
                fragColor = texture(uTexture, vTexCoord);
            }
        """.trimIndent()
    }
}
