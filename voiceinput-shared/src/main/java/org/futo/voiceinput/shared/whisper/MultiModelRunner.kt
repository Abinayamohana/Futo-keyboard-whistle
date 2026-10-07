package org.futo.voiceinput.shared.whisper

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import org.futo.voiceinput.shared.whistle.BailLanguageException
import org.futo.voiceinput.shared.whistle.DecodingMode
import org.futo.voiceinput.shared.whistle.InferenceCancelledException
import org.futo.voiceinput.shared.types.InferenceState
import org.futo.voiceinput.shared.types.Language
import org.futo.voiceinput.shared.types.ModelInferenceCallback
import org.futo.voiceinput.shared.types.ModelLoader
import org.futo.voiceinput.shared.types.getLanguageFromWhisperString
import org.futo.voiceinput.shared.types.toWhisperString


data class MultiModelRunConfiguration(
    val primaryModel: ModelLoader,
    val languageSpecificModels: Map<Language, ModelLoader>
)

data class DecodingConfiguration(
    val glossary: List<String>,
    val languages: Set<Language>,
    val suppressSymbols: Boolean
)

class MultiModelRunner(
    private val modelManager: ModelManager
) {
    suspend fun preload(runConfiguration: MultiModelRunConfiguration) = coroutineScope {
        val jobs = mutableListOf<Job>()

        jobs.add(launch(Dispatchers.Default) {
            modelManager.obtainWhistleModel(runConfiguration.primaryModel)
        })

        if (runConfiguration.languageSpecificModels.count() < 2) {
            runConfiguration.languageSpecificModels.forEach {
                jobs.add(launch(Dispatchers.Default) {
                    modelManager.obtainWhistleModel(it.value)
                })
            }
        }

        jobs.forEach { it.join() }
    }

    @Throws(InferenceCancelledException::class)
    suspend fun run(
        samples: FloatArray,
        runConfiguration: MultiModelRunConfiguration,
        decodingConfiguration: DecodingConfiguration,
        callback: ModelInferenceCallback
    ): String = coroutineScope {
        callback.updateStatus(InferenceState.LoadingModel)
        val primaryModel = modelManager.obtainWhistleModel(runConfiguration.primaryModel)

        val allowedLanguages = decodingConfiguration.languages.map { it.toWhisperString() }.toTypedArray()
        val bailLanguages = runConfiguration.languageSpecificModels.filter { it.value != runConfiguration.primaryModel }.keys.map { it.toWhisperString() }.toTypedArray()

        val glossary = if(decodingConfiguration.glossary.isNotEmpty()) {
            "(Glossary: " + decodingConfiguration.glossary.joinToString(separator = ", ") + ")"
        } else {
            ""
        }

        val result = try {
            callback.updateStatus(InferenceState.Encoding)
            primaryModel.infer(
                samples = samples,
                prompt = glossary,
                languages = allowedLanguages,
                bailLanguages = bailLanguages,
                decodingMode = DecodingMode.BeamSearch5,
                suppressNonSpeechTokens = decodingConfiguration.suppressSymbols,
                partialResultCallback = {
                    callback.partialResult(it)
                }
            )
        } catch(e: BailLanguageException) {
            callback.updateStatus(InferenceState.SwitchingModel)
            val language = getLanguageFromWhisperString(e.language)

            val specificModelLoader = runConfiguration.languageSpecificModels[language]!!
            val specificModel = modelManager.obtainWhistleModel(specificModelLoader)

            specificModel.infer(
                samples = samples,
                prompt = glossary,
                languages = arrayOf(e.language),
                bailLanguages = arrayOf(),
                decodingMode = DecodingMode.BeamSearch5,
                suppressNonSpeechTokens = decodingConfiguration.suppressSymbols,
                partialResultCallback = {
                    callback.partialResult(it)
                }
            )
        }

        return@coroutineScope result
    }

    fun cancelAll() {
        modelManager.cancelAll()
    }
}