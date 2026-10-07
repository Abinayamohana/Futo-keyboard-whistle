#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <jni.h>

#if defined(__aarch64__) || defined(__arm__)
#include "needle.h"
#define HAVE_NEEDLE 1
#else
#define HAVE_NEEDLE 0
#endif

#include "defines.h"
#include "org_futo_voiceinput_Whistle.h"
#include "jni_common.h"
#include "jni_utils.h"

#if defined(__arm__)
// Compatibility polyfill for 32-bit NDK libc++ where __hash_memory is expected by prebuilt libneedle.a
namespace std {
namespace __ndk1 {
size_t __hash_memory(const void* ptr, size_t n) noexcept {
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = static_cast<uint32_t>(n);
    const unsigned char* data = static_cast<const unsigned char*>(ptr);
    while (n >= 4) {
        uint32_t k = *(reinterpret_cast<const uint32_t*>(data));
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        n -= 4;
    }
    switch (n) {
        case 3: h ^= data[2] << 16; [[fallthrough]];
        case 2: h ^= data[1] << 8;  [[fallthrough]];
        case 1: h ^= data[0]; h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}
}
}
#endif

struct WhistleModelState {
    JNIEnv *env = nullptr;
    jobject partial_result_instance = nullptr;
    jmethodID partial_result_method = nullptr;
    void *buffer_address = nullptr;
    jlong buffer_capacity = 0;
    std::vector<unsigned char> file_buffer;
    volatile int cancel_flag = 0;
};

static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t key_pos = json.find(pattern);
    if (key_pos == std::string::npos) return "";

    size_t colon_pos = json.find(':', key_pos + pattern.length());
    if (colon_pos == std::string::npos) return "";

    size_t quote_start = json.find('\"', colon_pos + 1);
    if (quote_start == std::string::npos) return "";

    std::string result;
    bool escaped = false;
    for (size_t i = quote_start + 1; i < json.length(); ++i) {
        char c = json[i];
        if (escaped) {
            switch (c) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case '\"': result += '\"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'u':
                    if (i + 4 < json.length()) {
                        std::string hex_str = json.substr(i + 1, 4);
                        try {
                            int code_point = std::stoi(hex_str, nullptr, 16);
                            if (code_point < 0x80) {
                                result += static_cast<char>(code_point);
                            } else if (code_point < 0x800) {
                                result += static_cast<char>(0xC0 | (code_point >> 6));
                                result += static_cast<char>(0x80 | (code_point & 0x3F));
                            } else {
                                result += static_cast<char>(0xE0 | (code_point >> 12));
                                result += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (code_point & 0x3F));
                            }
                        } catch (...) {}
                        i += 4;
                    }
                    break;
                default:
                    result += c;
                    break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '\"') {
            break;
        } else {
            result += c;
        }
    }
    return result;
}

static jlong Whistle_open(JNIEnv *env, jclass clazz, jstring model_dir) {
    std::string path_str = jstring2string(env, model_dir);
    AKLOGI("Attempting to load Whistle model from file: %s", path_str.c_str());

    FILE *f = fopen(path_str.c_str(), "rb");
    if (!f) {
        AKLOGE("Failed to open Whistle model file: %s", path_str.c_str());
        return 0L;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    auto *state = new WhistleModelState();
    state->file_buffer.resize(fsize);
    size_t read_bytes = fread(state->file_buffer.data(), 1, fsize, f);
    fclose(f);

    if (read_bytes != static_cast<size_t>(fsize)) {
        AKLOGE("Failed to read Whistle model file: %s", path_str.c_str());
        delete state;
        return 0L;
    }

#if HAVE_NEEDLE
    int ret = needle_load(state->file_buffer.data(), static_cast<unsigned long long>(fsize));
    if (ret < 0) {
        AKLOGE("needle_load failed for file %s: %s (code %d)", path_str.c_str(), needle_last_error(), ret);
        delete state;
        return 0L;
    }
    int models = needle_models();
    (void)models;
    AKLOGI("Whistle loaded via Needle successfully from file! Models: %d", models);
    return reinterpret_cast<jlong>(state);
#else
    delete state;
    return 0L;
#endif
}

static jlong Whistle_openFromBuffer(JNIEnv *env, jclass clazz, jobject buffer) {
    void* buffer_address = env->GetDirectBufferAddress(buffer);
    jlong buffer_capacity = env->GetDirectBufferCapacity(buffer);

    AKLOGI("Attempting to load Whistle model from direct buffer (capacity: %ld bytes)...", buffer_capacity);

    if (!buffer_address || buffer_capacity <= 0) {
        AKLOGE("Invalid buffer provided to Whistle_openFromBuffer");
        return 0L;
    }

    auto *state = new WhistleModelState();
    state->buffer_address = buffer_address;
    state->buffer_capacity = buffer_capacity;

#if HAVE_NEEDLE
    int ret = needle_load(
        reinterpret_cast<const unsigned char*>(buffer_address),
        static_cast<unsigned long long>(buffer_capacity)
    );
    if (ret < 0) {
        AKLOGE("needle_load failed: %s (code %d)", needle_last_error(), ret);
        delete state;
        return 0L;
    }

    int models = needle_models();
    (void)models;
    AKLOGI("Whistle model loaded with Needle C++ engine (16.9 MB whistle.cact). needle_models=%d (NEEDLE_SPEECH=%d)", models, NEEDLE_SPEECH);
    return reinterpret_cast<jlong>(state);
#else
    AKLOGE("Needle C++ engine not compiled for this architecture");
    delete state;
    return 0L;
#endif
}

static jstring Whistle_infer(JNIEnv *env, jobject instance, jlong handle, jfloatArray samples_array, jstring prompt, jobjectArray languages, jobjectArray bail_languages, jint decoding_mode, jboolean suppress_non_speech_tokens) {
    AKLOGI("Whistle_infer starting execution via Needle engine...");

    auto *state = reinterpret_cast<WhistleModelState *>(handle);
    if (!state) {
        AKLOGE("Invalid Whistle handle passed to infer");
        return string2jstring(env, "");
    }

    state->cancel_flag = 0;

    // Primary language
    std::string primary_language = "";
    int num_languages = env->GetArrayLength(languages);
    if (num_languages > 0) {
        jstring jstr = static_cast<jstring>(env->GetObjectArrayElement(languages, 0));
        primary_language = jstring2string(env, jstr);
    }

    // Bail languages
    std::vector<std::string> forbidden_languages;
    int num_bail_languages = env->GetArrayLength(bail_languages);
    for (int i = 0; i < num_bail_languages; i++) {
        jstring jstr = static_cast<jstring>(env->GetObjectArrayElement(bail_languages, i));
        forbidden_languages.push_back(jstring2string(env, jstr));
    }

    size_t num_samples = env->GetArrayLength(samples_array);
    jfloat *samples = env->GetFloatArrayElements(samples_array, nullptr);

    std::string prompt_str = jstring2string(env, prompt);
    const char *prompt_cstr = prompt_str.empty() ? nullptr : prompt_str.c_str();

    // Needle supports "en", "de", "fr", "es", "it", "nl", "pl", or NULL to auto-detect
    const char *lang_cstr = nullptr;
    if (!primary_language.empty()) {
        if (primary_language == "en" || primary_language == "de" || primary_language == "fr" ||
            primary_language == "es" || primary_language == "it" || primary_language == "nl" ||
            primary_language == "pl") {
            lang_cstr = primary_language.c_str();
        }
    }

    state->env = env;
    state->partial_result_instance = instance;
    state->partial_result_method = env->GetMethodID(
            env->GetObjectClass(instance),
            "invokePartialResult",
            "(Ljava/lang/String;)V");

    std::string output = "";

#if HAVE_NEEDLE
    if (state->cancel_flag) {
        env->ReleaseFloatArrayElements(samples_array, samples, JNI_ABORT);
        return string2jstring(env, "<>CANCELLED<> flag");
    }

    // needle_transcribe processes up to 30s (480000 samples) per call
    const size_t MAX_SAMPLES_PER_PASS = 480000;
    size_t offset = 0;
    std::string detected_lang_all = "";

    while (offset < num_samples && !state->cancel_flag) {
        size_t chunk_size = std::min(num_samples - offset, MAX_SAMPLES_PER_PASS);
        char out_buf[65536] = {0};

        AKLOGI("Calling needle_transcribe: offset %zu, samples %zu (total %zu, lang=%s)...",
               offset, chunk_size, num_samples, lang_cstr ? lang_cstr : "auto");

        int tokens = needle_transcribe(
            samples + offset,
            static_cast<int>(chunk_size),
            lang_cstr,
            prompt_cstr,
            0,
            out_buf,
            static_cast<int>(sizeof(out_buf))
        );

        if (tokens < 0) {
            const char* err = needle_last_error();
            (void)err;
            AKLOGE("needle_transcribe error code %d: %s", tokens, err ? err : "unknown");
            break;
        }

        AKLOGI("needle_transcribe result (tokens=%d): %s", tokens, out_buf);
        std::string chunk_text = extract_json_string(out_buf, "text");
        std::string detected_lang = extract_json_string(out_buf, "language");
        if (detected_lang_all.empty()) {
            detected_lang_all = detected_lang;
        }

        // Check if detected language is in bail list
        bool bail = false;
        for (const auto& bl : forbidden_languages) {
            if (detected_lang == bl) {
                bail = true;
                output = "<>CANCELLED<> lang=" + detected_lang;
                break;
            }
        }
        if (bail) break;

        if (!chunk_text.empty()) {
            if (!output.empty() && output.back() != ' ' && chunk_text.front() != ' ') {
                output += " ";
            }
            output += chunk_text;

            // Send partial result callback to UI
            if (state->partial_result_instance && state->partial_result_method) {
                jstring partial_jstr = string2jstring(env, output.c_str());
                env->CallVoidMethod(state->partial_result_instance, state->partial_result_method, partial_jstr);
                env->DeleteLocalRef(partial_jstr);
            }
        }

        offset += chunk_size;
    }
#else
    AKLOGE("Needle C++ engine not compiled for this architecture");
#endif

    env->ReleaseFloatArrayElements(samples_array, samples, JNI_ABORT);

    if (state->cancel_flag) {
        output = "<>CANCELLED<> flag";
    }

    AKLOGI("Whistle_infer completed! Final transcribed output: \"%s\"", output.c_str());
    return string2jstring(env, output.c_str());
}

static void Whistle_cancel(JNIEnv *env, jclass clazz, jlong handle) {
    auto *state = reinterpret_cast<WhistleModelState *>(handle);
    if (state) {
        state->cancel_flag = 1;
    }
}

static void Whistle_close(JNIEnv *env, jclass clazz, jlong handle) {
    auto *state = reinterpret_cast<WhistleModelState *>(handle);
    if (!state) return;
#if HAVE_NEEDLE
    needle_reset();
#endif
    delete state;
}

static const JNINativeMethod sMethods[] = {
        {
                const_cast<char *>("openNative"),
                const_cast<char *>("(Ljava/lang/String;)J"),
                reinterpret_cast<void *>(Whistle_open)
        },
        {
                const_cast<char *>("openFromBufferNative"),
                const_cast<char *>("(Ljava/nio/Buffer;)J"),
                reinterpret_cast<void *>(Whistle_openFromBuffer)
        },
        {
                const_cast<char *>("inferNative"),
                const_cast<char *>("(J[FLjava/lang/String;[Ljava/lang/String;[Ljava/lang/String;IZ)Ljava/lang/String;"),
                reinterpret_cast<void *>(Whistle_infer)
        },
        {
                const_cast<char *>("cancelNative"),
                const_cast<char *>("(J)V"),
                reinterpret_cast<void *>(Whistle_cancel)
        },
        {
                const_cast<char *>("closeNative"),
                const_cast<char *>("(J)V"),
                reinterpret_cast<void *>(Whistle_close)
        }
};

namespace voiceinput {
    int register_Whistle(JNIEnv *env) {
        const char *const kClassPathName = "org/futo/voiceinput/shared/whistle/Whistle";
        return latinime::registerNativeMethods(env, kClassPathName, sMethods, NELEMS(sMethods));
    }
}
