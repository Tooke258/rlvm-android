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
        resolver.query(
            DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, id),
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null,
            null,
            null
        )?.use { cursor ->
            while (cursor.moveToNext()) {
                cursor.getString(0)?.let(names::add)
            }
        }
        return names.toTypedArray()
    }

    /**
     * detachFd() 把 fd 的所有权交给 native；native 侧 mmap 之后立即 close()，
     * 因此不会泄漏描述符。
     */
    fun openFd(relPath: String): Int {
        val id = resolveDocumentId(relPath) ?: return -1
        val pfd = resolver.openFileDescriptor(buildDocumentUri(id), "r") ?: return -1
        return pfd.detachFd()
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
        return null
    }
}
