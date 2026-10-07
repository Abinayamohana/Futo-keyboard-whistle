package org.futo.voiceinput.shared.whistle

import androidx.annotation.Keep
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.newSingleThreadContext
import kotlinx.coroutines.withContext
import java.nio.Buffer

enum class DecodingMode(val value: Int) {
    Greedy(0),
    BeamSearch5(5)
}

class BailLanguageException(val language: String) : Exception()
class InferenceCancelledException : Exception()
class InvalidModelException : Exception("The Whistle model could not be loaded from the given buffer")

@OptIn(DelicateCoroutinesApi::class)
val whistleInferenceContext = newSingleThreadContext("whistle-inference")

@Keep
class Whistle(
    modelBuffer: Buffer
) {
    private var handle: Long = 0L

    init {
        handle = openFromBufferNative(modelBuffer)
        if (handle == 0L) {
            throw InvalidModelException()
        }
    }

    private var partialResultCallback: (String) -> Unit = { }

    @Keep
    private fun invokePartialResult(text: String) {
        partialResultCallback(text.trim())
    }

    @Throws(BailLanguageException::class, InferenceCancelledException::class)
    suspend fun infer(
        samples: FloatArray,
        prompt: String,
        languages: Array<String>,
        bailLanguages: Array<String>,
        decodingMode: DecodingMode,
        suppressNonSpeechTokens: Boolean,
        partialResultCallback: (String) -> Unit
    ): String = withContext(whistleInferenceContext) {
        if (handle == 0L) {
            throw IllegalStateException("Whistle has already been closed, cannot infer")
        }
        this@Whistle.partialResultCallback = partialResultCallback

        val audioDurationSec = samples.size / 16000.0f
        val startTimeMs = System.currentTimeMillis()

        android.util.Log.i("Whistle", String.format(java.util.Locale.US, "Whistle inference starting: %d samples (%.2fs audio)", samples.size, audioDurationSec))

        val result = inferNative(handle, samples, prompt, languages, bailLanguages, decodingMode.value, suppressNonSpeechTokens).trim()

        val elapsedMs = System.currentTimeMillis() - startTimeMs
        val inferenceTimeSec = elapsedMs / 1000.0f
        val rtf = if (audioDurationSec > 0) inferenceTimeSec / audioDurationSec else 0.0f
        val isCancelled = result.contains("<>CANCELLED<>")
        val wordCount = if (result.isEmpty() || isCancelled) 0 else result.split("\\s+".toRegex()).filter { it.isNotEmpty() }.size
        val wordsPerSec = if (inferenceTimeSec > 0) wordCount / inferenceTimeSec else 0.0f

        val metricsLog = String.format(
            java.util.Locale.US,
            """
            |================ WHISTLE TRANSCRIPTION METRICS ================
            |Transcription Result : "%s"
            |Audio Duration       : %.2f seconds (%d samples)
            |Inference Latency    : %d ms (%.2f seconds)
            |Real-Time Factor     : %.3fx %s
            |Word Count           : %d words
            |Transcription Speed  : %.2f words/sec
            |===============================================================
            """.trimMargin(),
            if (isCancelled) "<CANCELLED>" else result,
            audioDurationSec, samples.size,
            elapsedMs, inferenceTimeSec,
            rtf, if (rtf < 1.0f) "(Faster than real-time)" else "(Slower than real-time)",
            wordCount,
            wordsPerSec
        )

        android.util.Log.i("Whistle", metricsLog)

        if (result.contains("<>CANCELLED<>")) {
            if (result.contains("flag")) {
                throw InferenceCancelledException()
            } else if (result.contains("lang=")) {
                val language = result.split("lang=")[1]
                throw BailLanguageException(language)
            } else {
                throw IllegalStateException("Cancelled for unknown reason")
            }
        } else {
            return@withContext result
        }
    }

    fun cancel() {
        if (handle == 0L) return
        cancelNative(handle)
    }

    suspend fun close() = withContext(whistleInferenceContext) {
        if (handle != 0L) {
            closeNative(handle)
        }
        handle = 0L
    }

    private external fun openNative(path: String): Long
    private external fun openFromBufferNative(buffer: Buffer): Long
    private external fun inferNative(
        handle: Long,
        samples: FloatArray,
        prompt: String,
        languages: Array<String>,
        bailLanguages: Array<String>,
        decodingMode: Int,
        suppressNonSpeechTokens: Boolean
    ): String
    private external fun cancelNative(handle: Long)
    private external fun closeNative(handle: Long)
}
