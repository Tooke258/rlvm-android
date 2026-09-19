package org.rlvm.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract

/**
 * 暴露给 native 的最小 SAF 后端。
 *
 * task.md 要求 Android 12+ 的外部文件访问一律通过 SAF + DocumentFile，禁止直接拼 File 路径。
 * native 侧的 SafBackend 通过 JNI 回调这里的同名方法，所有路径都相对于
 * 用户在系统选择器中授权的目录树。
 *
 * 只做「解析相对路径 -> document uri -> ContentResolver 查询/fd」这一件事，
 * 不缓存目录结构：DocumentFile 的树可能很大，按需查询更可控。
 */
class SafFileSystem(private val context: Context, private val treeUri: Uri) {

    private val resolver get() = context.contentResolver

    private val treeDocumentId: String = DocumentsContract.getTreeDocumentId(treeUri)

    private fun documentId(relPath: String): String =
        if (relPath.isEmpty()) treeDocumentId else "$treeDocumentId/$relPath"

    private fun documentUri(relPath: String): Uri =
        DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId(relPath))

    /** 对应 native 的 SafBackend::Exists */
    fun exists(relPath: String): Boolean = query(relPath) != null

    /** 对应 native 的 SafBackend::IsDirectory */
    fun isDirectory(relPath: String): Boolean =
        query(relPath)?.mimeType == DocumentsContract.Document.MIME_TYPE_DIR

    /** 对应 native 的 SafBackend::Size，未知返回 -1 */
    fun size(relPath: String): Long = query(relPath)?.size ?: -1L

    /** 对应 native 的 SafBackend::ListDirectory */
    fun listDirectory(relPath: String): Array<String> {
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
            treeUri,
            documentId(relPath)
        )
        val names = ArrayList<String>()
        resolver.query(
            childrenUri,
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
     * 对应 native 的 SafBackend::OpenFd。
     *
     * detachFd() 把 fd 的所有权交给 native；native 侧 mmap 之后立即 close()，
     * 因此不会泄漏描述符。
     */
    fun openFd(relPath: String): Int {
        val pfd = resolver.openFileDescriptor(documentUri(relPath), "r") ?: return -1
        return pfd.detachFd()
    }

    private class DocInfo(val mimeType: String, val size: Long)

    private fun query(relPath: String): DocInfo? {
        val uri = documentUri(relPath)
        resolver.query(
            uri,
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
