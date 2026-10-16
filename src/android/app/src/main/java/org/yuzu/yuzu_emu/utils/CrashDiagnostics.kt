// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.Context
import android.os.Build
import android.util.AtomicFile
import androidx.annotation.RequiresApi
import java.io.File
import java.io.IOException
import java.time.Instant
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.getPublicFilesDir

object CrashDiagnostics {
    private const val MAX_EXITS = 4
    private const val MAX_TRACE_BYTES = 8 * 1024 * 1024
    private const val REPORT_NAME = "android_exit_info.txt"
    private const val TRACE_PREFIX = "android_exit_"
    private const val BUNDLE_NAME = "eden_diagnostics.zip"
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var collection: Job? = null
    private val bundleMutex = Mutex()

    fun start(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || collection != null) {
            return
        }
        val appContext = context.applicationContext
        recordSession(appContext, "startup")
        collection = scope.launch {
            try {
                collect(appContext)
            } catch (e: Exception) {
                android.util.Log.w("CrashDiagnostics", "Could not collect previous process exits", e)
            }
        }
    }

    fun recordSession(context: Context, phase: String, programId: String = "") {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val summary = "build=${BuildConfig.GIT_REVISION.take(40)};" +
            "phase=${phase.take(24)};title=${programId.take(16)}"
        val bytes = summary.toByteArray(Charsets.UTF_8)
        try {
            context.getSystemService(ActivityManager::class.java)?.setProcessStateSummary(
                bytes.copyOf(minOf(bytes.size, 128))
            )
        } catch (e: RuntimeException) {
            android.util.Log.w("CrashDiagnostics", "Could not record process session", e)
        }
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun collect(context: Context) {
        val manager = context.getSystemService(ActivityManager::class.java) ?: return
        val exits = manager.getHistoricalProcessExitReasons(context.packageName, 0, MAX_EXITS)
        val directory = File(context.getPublicFilesDir(), "log").apply { mkdirs() }
        val retainedTraces = mutableSetOf<String>()
        val report = buildString {
            appendLine("Android process exit history (most recent first)")
            appendLine("Collected at: ${Instant.now()}")
            appendLine("Collector build: ${BuildConfig.VERSION_NAME}; commit: ${BuildConfig.GIT_REVISION}")
            appendLine("Low-memory kill reporting supported: ${ActivityManager.isLowMemoryKillReportSupported()}")
            appendLine("PSS/RSS are Android's last samples, not necessarily memory at the crash.")
            appendLine("A SIGKILL alone does not establish an out-of-memory kill.")
            if (exits.isEmpty()) {
                appendLine("No previous exit records available.")
            }
            for (exit in exits) {
                appendLine()
                appendLine("Timestamp: ${Instant.ofEpochMilli(exit.timestamp)}")
                appendLine("Process: ${exit.processName}; PID: ${exit.pid}")
                val session = exit.processStateSummary?.toString(Charsets.UTF_8)
                appendLine("Previous build/session: ${session ?: "unavailable from this build"}")
                appendLine("Reason: ${reasonName(exit.reason)} (${exit.reason}); status/signal: ${exit.status}")
                appendLine("PSS: ${exit.pss} KiB; RSS: ${exit.rss} KiB; importance: ${exit.importance}")
                appendLine("Description: ${exit.description.orEmpty()}")
                val suffix = if (exit.reason == ApplicationExitInfo.REASON_CRASH_NATIVE &&
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                ) "pb" else "txt"
                val trace = File(directory, "$TRACE_PREFIX${exit.timestamp}_${exit.pid}.$suffix")
                try {
                    if (saveTrace(exit, trace)) {
                        retainedTraces.add(trace.name)
                        appendLine("Trace: ${trace.name}")
                    } else {
                        appendLine("Trace: unavailable from Android")
                    }
                } catch (e: IOException) {
                    appendLine("Trace: not saved (${e.message})")
                }
            }
        }
        val reportFile = AtomicFile(File(directory, REPORT_NAME))
        val output = reportFile.startWrite()
        try {
            output.write(report.toByteArray(Charsets.UTF_8))
            reportFile.finishWrite(output)
        } catch (e: Exception) {
            reportFile.failWrite(output)
            throw e
        }
        directory.listFiles()?.filter {
            it.name.startsWith(TRACE_PREFIX) && it.name != REPORT_NAME &&
                (it.extension == "pb" || it.extension == "txt" || it.extension == "tmp") &&
                it.name !in retainedTraces
        }?.forEach { it.delete() }
    }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun saveTrace(exit: ApplicationExitInfo, target: File): Boolean {
        if (target.isFile && target.length() in 1..MAX_TRACE_BYTES.toLong()) {
            return true
        }
        val input = exit.traceInputStream ?: return false
        val temporary = File(target.parentFile, "${target.name}.tmp")
        try {
            input.use { source ->
                temporary.outputStream().use { destination ->
                    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                    var total = 0
                    while (true) {
                        val count = source.read(buffer)
                        if (count < 0) break
                        total += count
                        if (total > MAX_TRACE_BYTES) {
                            throw IOException("trace exceeds $MAX_TRACE_BYTES bytes")
                        }
                        destination.write(buffer, 0, count)
                    }
                }
            }
            if (temporary.length() == 0L) return false
            if (!temporary.renameTo(target)) throw IOException("could not save trace")
            return true
        } finally {
            temporary.delete()
        }
    }

    suspend fun createLogBundle(context: Context, logName: String): File? =
        withContext(Dispatchers.IO) {
            collection?.join()
            bundleMutex.withLock {
                val directory = File(context.getPublicFilesDir(), "log")
                val report = File(directory, REPORT_NAME)
                if (!report.isFile) return@withLock null
                val target = File(directory, BUNDLE_NAME)
                val archive = AtomicFile(target)
                var output: java.io.FileOutputStream? = null
                try {
                    output = archive.startWrite()
                    ZipOutputStream(output).use { zip ->
                        val files = listOf(File(directory, logName), report) +
                            directory.listFiles().orEmpty().filter {
                                it.name.startsWith(TRACE_PREFIX) && it.name != REPORT_NAME &&
                                    (it.extension == "pb" || it.extension == "txt")
                            }.sortedByDescending { it.name }.take(MAX_EXITS)
                        for (file in files) {
                            if (!file.isFile) continue
                            zip.putNextEntry(ZipEntry(file.name))
                            file.inputStream().use { it.copyTo(zip) }
                            zip.closeEntry()
                        }
                        zip.finish()
                        archive.finishWrite(output)
                    }
                    target
                } catch (e: Exception) {
                    archive.failWrite(output)
                    android.util.Log.w("CrashDiagnostics", "Could not package diagnostics", e)
                    null
                }
            }
        }

    @RequiresApi(Build.VERSION_CODES.R)
    private fun reasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_ANR -> "ANR"
        ApplicationExitInfo.REASON_CRASH -> "Java crash"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "Native crash"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "Low-memory kill"
        ApplicationExitInfo.REASON_SIGNALED -> "Signaled"
        ApplicationExitInfo.REASON_EXIT_SELF -> "Self exit"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "User requested"
        ApplicationExitInfo.REASON_USER_STOPPED -> "User stopped"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "Initialization failure"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "Excessive resource usage"
        else -> "Other/unknown"
    }
}
