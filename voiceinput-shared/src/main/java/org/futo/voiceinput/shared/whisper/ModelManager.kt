package org.futo.voiceinput.shared.whisper

import android.content.Context
import org.futo.voiceinput.shared.types.ModelLoader
import org.futo.voiceinput.shared.whistle.Whistle

class ModelManager(
    val context: Context
) {
    private val loadedWhistleModels: HashMap<Any, Whistle> = hashMapOf()

    fun obtainWhistleModel(model: ModelLoader): Whistle {
        val key = model.key(context)
        if (!loadedWhistleModels.contains(key)) {
            loadedWhistleModels[key] = model.loadWhistle(context)
        }

        return loadedWhistleModels[key]!!
    }

    fun cancelAll() {
        loadedWhistleModels.forEach {
            it.value.cancel()
        }
    }

    suspend fun cleanUp() {
        for (model in loadedWhistleModels.entries) {
            model.value.cancel()
            model.value.close()
        }
        loadedWhistleModels.clear()
    }
}
