package org.futo.voiceinput.shared

import org.futo.voiceinput.shared.types.ModelBuiltInAsset
import org.futo.voiceinput.shared.types.ModelDownloadable
import org.futo.voiceinput.shared.types.ModelLoader

val BUILTIN_WHISTLE_MODEL: ModelLoader = ModelBuiltInAsset(
    name = R.string.tiny_en_name,
    modelFile = "whistle.cact"
)

val WHISTLE_MODELS: List<ModelLoader> = listOf(
    ModelDownloadable(
        name = R.string.tiny_en_name,
        modelFile = "whistle.cact",
        checksum = "b6e02f048568ac5d01a2042556c658061e699acbc0aa2a1439f52f3d461dffeb"
    )
)