package org.rlvm.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract

/**
 * 暴露给 native 的最小 SAF 后端。
 *
 * task.md 要求 Android 12+ 的外部文件访问一律通过 SAF，禁止直接拼 File 路径。
 * native 侧的 SafBackend 通过 JNI 回调这里的同名方法。
 *
 * ## 为什么需要大小写不敏感解析
 *
 * RealLive 游戏原本跑在 Windows / FAT 上，文件名大小写不敏感，落到手机存储后可能变成
 * `seen.txt`、`gameexe.ini`、`seen0002.txt`，甚至同一个游戏里混用不同风格。
 * 而 SAF 的 documentId 是 **精确匹配** 的字符串，直接 `parent + "/" + name` 会找不到。
 *
 * 上游为此写了 `CorrectPathCase()`；这里是同一套语义在 SAF 上的实现：逐级解析路径，
 * 每一级先走精确匹配的快路径，失败再列目录按 display name 做大小写不敏感比对。
 * 解析结果按请求路径缓存（含失败结果），因此重复访问不会反复列目录。
 */
class SafFileSystem(private val context: Context, private val treeUri: Uri) {

    private val resolver get() = context.contentResolver

    private val treeDocumentId: String = DocumentsContract.getTreeDocumentId(treeUri)

    /** 请求的相对路径 -> 已解析的 documentId；值为 null 表示解析失败（负缓存）。 */
    private val resolved = HashMap<String, String?>()

    // -- native 回调 --------------------------------------------------------

    fun exists(relPath: String): Boolean = resolveDocumentId(relPath) != null

    fun isDirectory(relPath: String): Boolean {
        val id = resolveDocumentId(relPath) ?: return false
        return info(id)?.mimeType == DocumentsContract.Document.MIME_TYPE_DIR
    }

    fun size(relPath: String): Long {
        val id = resolveDocumentId(relPath) ?: return -1L
        return info(id)?.size ?: -1L
    }

    fun listDirectory(relPath: String): Array<String> {
        val id = resolveDocumentId(relPath) ?: return emptyArray()
        val names = ArrayList<String>()
        try {
            resolver.query(
                DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, id),
                arrayOf(
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE
                ),
                null,
                null,
                null
            )?.use { cursor ->
                while (cursor.moveToNext()) {
                    val name = cursor.getString(0) ?: continue
                    val isDirectory =
                        cursor.getString(1) == DocumentsContract.Document.MIME_TYPE_DIR
                    // 编码成 "D\tname" / "F\tname"：一次 JNI 调用就把类型带回 native，
                    // 省掉每个文件一次跨进程的 IsDirectory 查询。
                    // 真实游戏目录有数千个文件，逐文件查询会让索引构建慢到不可用。
                    names.add((if (isDirectory) "D\t" else "F\t") + name)
                }
            }
        } catch (e: Exception) {
            // 静默返回已收集到的部分；异常绝不能穿到 JNI 之外。
        }
        return names.toTypedArray()
    }

    /**
     * detachFd() 把 fd 的所有权交给 native；native 侧 mmap 之后立即 close()，
     * 因此不会泄漏描述符。
     */
    fun openFd(relPath: String): Int {
        val id = resolveDocumentId(relPath) ?: return -1
        return try {
            val pfd = resolver.openFileDescriptor(buildDocumentUri(id), "r")
            pfd?.detachFd() ?: -1
        } catch (e: Exception) {
            // openFileDescriptor 对不存在的文档会抛 FileNotFoundException。
            // 必须在这里吞掉：异常穿过 JNI 会让返回值变成未定义值
            //（实测表现为「不存在的文件返回了看似有效的 fd」），
            // 而且挂起的异常会污染后续所有 JNI 调用。
            -1
        }
    }

    /**
     * 创建（或截断）relPath 对应的文件，返回可写 fd；失败返回 -1。
     *
     * 存档与全局数据（Config）走这条：SAF 下没有真实路径，只能由
     * ContentResolver 建文档。父目录不存在时逐级创建——游戏首次存档时
     * SAVEDATA/ 可能还不存在。
     */
    fun createFd(relPath: String): Int {
        val parts = relPath.split('/').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return -1
        return try {
            var parentId = treeDocumentId
            for (i in 0 until parts.size - 1) {
                parentId = ensureDirectory(parentId, parts[i]) ?: return -1
            }
            val name = parts.last()
            val docId = findChildId(parentId, name) ?: run {
                val created = DocumentsContract.createDocument(
                    resolver, buildDocumentUri(parentId),
                    "application/octet-stream", name
                ) ?: return -1
                DocumentsContract.getDocumentId(created)
            }
            val pfd = resolver.openFileDescriptor(buildDocumentUri(docId), "rw") ?: return -1
            pfd.detachFd()
        } catch (e: Exception) {
            -1
        }
    }

    /** 取子目录的文档 id，不存在则创建。 */
    private fun ensureDirectory(parentId: String, name: String): String? {
        findChildId(parentId, name)?.let { return it }
        return try {
            val uri = DocumentsContract.createDocument(
                resolver, buildDocumentUri(parentId),
                DocumentsContract.Document.MIME_TYPE_DIR, name
            ) ?: return null
            DocumentsContract.getDocumentId(uri)
        } catch (e: Exception) {
            null
        }
    }

    // -- 路径解析 -----------------------------------------------------------

    private fun buildDocumentUri(documentId: String): Uri =
        DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId)

    private fun resolveDocumentId(relPath: String): String? {
        if (relPath.isEmpty()) return treeDocumentId
        if (resolved.containsKey(relPath)) return resolved[relPath]

        var currentId = treeDocumentId
        for (segment in relPath.split('/')) {
            if (segment.isEmpty()) continue
            val childId = findChildId(currentId, segment)
            if (childId == null) {
                resolved[relPath] = null
                return null
            }
            currentId = childId
        }
        resolved[relPath] = currentId
        return currentId
    }

    private fun findChildId(parentId: String, name: String): String? {
        // 快路径：标准 provider 的子 documentId 恰好是 parent/name，一次查询即可。
        val exactId = "$parentId/$name"
        if (info(exactId) != null) return exactId

        // 回退：列目录做大小写不敏感匹配。
        resolver.query(
            DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId),
            arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME
            ),
            null,
            null,
            null
        )?.use { cursor ->
            while (cursor.moveToNext()) {
                val id = cursor.getString(0) ?: continue
                val displayName = cursor.getString(1) ?: continue
                if (displayName.equals(name, ignoreCase = true)) return id
            }
        }
        return null
    }

    private class DocInfo(val mimeType: String, val size: Long)

    private fun info(documentId: String): DocInfo? {
        try {
            resolver.query(
                buildDocumentUri(documentId),
                arrayOf(
                    DocumentsContract.Document.COLUMN_MIME_TYPE,
                    DocumentsContract.Document.COLUMN_SIZE
                ),
                null,
                null,
                null
            )?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val mime = cursor.getString(0) ?: return null
                    val size = if (cursor.isNull(1)) -1L else cursor.getLong(1)
                    return DocInfo(mime, size)
                }
            }
        } catch (e: Exception) {
            return null
        }
        return null
    }
}
