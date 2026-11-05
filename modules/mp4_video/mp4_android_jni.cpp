/**************************************************************************/
/*  mp4_android_jni.cpp                                                   */
/*                                                                        */
/*  JNI bridge to the Android MediaCodec based MP4 decoder.               */
/**************************************************************************/

#include "mp4_android_jni.h"

#include "core/io/file_access.h"
#include "core/os/memory.h"

#ifdef ANDROID_ENABLED

#include "platform/android/jni_utils.h"

#include <jni.h>

struct MP4VideoDecoderHandle {
	jobject object = nullptr;
	const uint8_t *yuv_ptr = nullptr;
	const float *audio_ptr = nullptr;
	int channels = 0;
	int last_serial = -1;

	jmethodID m_play = nullptr;
	jmethodID m_stop = nullptr;
	jmethodID m_set_paused = nullptr;
	jmethodID m_is_playing = nullptr;
	jmethodID m_seek = nullptr;
	jmethodID m_get_duration = nullptr;
	jmethodID m_get_position = nullptr;
	jmethodID m_get_video_width = nullptr;
	jmethodID m_get_video_height = nullptr;
	jmethodID m_get_channels = nullptr;
	jmethodID m_get_sample_rate = nullptr;
	jmethodID m_update = nullptr;
	jmethodID m_mix_audio = nullptr;
	jmethodID m_get_frame_width = nullptr;
	jmethodID m_get_frame_height = nullptr;
	jmethodID m_get_rotation = nullptr;
	jmethodID m_get_frame_serial = nullptr;
	jmethodID m_release = nullptr;
};

static void _clear_exception(JNIEnv *p_env) {
	if (p_env->ExceptionCheck()) {
		p_env->ExceptionClear();
	}
}

static jmethodID _get_method(JNIEnv *p_env, jclass p_class, const char *p_name, const char *p_signature) {
	jmethodID method = p_env->GetMethodID(p_class, p_name, p_signature);
	if (!method) {
		_clear_exception(p_env);
	}
	return method;
}

MP4VideoDecoderHandle *mp4_decoder_create(const String &p_path, String *r_error) {
	if (r_error) {
		*r_error = String();
	}

	JNIEnv *env = get_jni_env();
	if (!env) {
		if (r_error) {
			*r_error = "JNI environment tidak tersedia";
		}
		return nullptr;
	}

	Error file_error = OK;
	Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path, &file_error);
	if (file_error != OK || bytes.is_empty()) {
		if (r_error) {
			*r_error = vformat("Gagal membaca file video: %s", p_path);
		}
		return nullptr;
	}

	jclass cls = jni_find_class(env, "org/godotengine/godot/mp4/Mp4VideoDecoder");
	if (!cls) {
		if (r_error) {
			*r_error = "Kelas Java Mp4VideoDecoder tidak ditemukan";
		}
		return nullptr;
	}

	jmethodID ctor = env->GetMethodID(cls, "<init>", "([B)V");
	if (!ctor) {
		_clear_exception(env);
		env->DeleteLocalRef(cls);
		if (r_error) {
			*r_error = "Konstruktor Mp4VideoDecoder tidak ditemukan";
		}
		return nullptr;
	}

	jbyteArray data = env->NewByteArray((jsize)bytes.size());
	env->SetByteArrayRegion(data, 0, (jsize)bytes.size(), reinterpret_cast<const jbyte *>(bytes.ptr()));
	jobject local = env->NewObject(cls, ctor, data);
	env->DeleteLocalRef(data);
	env->DeleteLocalRef(cls);

	if (env->ExceptionCheck() || !local) {
		_clear_exception(env);
		if (r_error) {
			*r_error = "Gagal membuat decoder MP4 (format/codec tidak didukung?)";
		}
		return nullptr;
	}

	jobject object = env->NewGlobalRef(local);
	env->DeleteLocalRef(local);
	if (!object) {
		if (r_error) {
			*r_error = "Gagal menyimpan referensi decoder MP4";
		}
		return nullptr;
	}

	MP4VideoDecoderHandle *handle = memnew(MP4VideoDecoderHandle);
	handle->object = object;

	jclass object_class = env->GetObjectClass(object);
	handle->m_play = _get_method(env, object_class, "play", "()V");
	handle->m_stop = _get_method(env, object_class, "stopPlayback", "()V");
	handle->m_set_paused = _get_method(env, object_class, "setPaused", "(Z)V");
	handle->m_is_playing = _get_method(env, object_class, "isPlaying", "()Z");
	handle->m_seek = _get_method(env, object_class, "seekTo", "(D)V");
	handle->m_get_duration = _get_method(env, object_class, "getDuration", "()D");
	handle->m_get_position = _get_method(env, object_class, "getPosition", "()D");
	handle->m_get_video_width = _get_method(env, object_class, "getVideoWidth", "()I");
	handle->m_get_video_height = _get_method(env, object_class, "getVideoHeight", "()I");
	handle->m_get_channels = _get_method(env, object_class, "getChannels", "()I");
	handle->m_get_sample_rate = _get_method(env, object_class, "getSampleRate", "()I");
	handle->m_update = _get_method(env, object_class, "update", "(D)Z");
	handle->m_mix_audio = _get_method(env, object_class, "mixAudio", "(I)I");
	handle->m_get_frame_width = _get_method(env, object_class, "getFrameWidth", "()I");
	handle->m_get_frame_height = _get_method(env, object_class, "getFrameHeight", "()I");
	handle->m_get_rotation = _get_method(env, object_class, "getRotation", "()I");
	handle->m_get_frame_serial = _get_method(env, object_class, "getFrameSerial", "()I");
	handle->m_release = _get_method(env, object_class, "release", "()V");

	jmethodID get_yuv = _get_method(env, object_class, "getYuvBuffer", "()Ljava/nio/ByteBuffer;");
	jmethodID get_audio = _get_method(env, object_class, "getAudioBuffer", "()Ljava/nio/ByteBuffer;");
	env->DeleteLocalRef(object_class);

	if (get_yuv) {
		jobject buffer = env->CallObjectMethod(object, get_yuv);
		if (!env->ExceptionCheck() && buffer) {
			handle->yuv_ptr = reinterpret_cast<const uint8_t *>(env->GetDirectBufferAddress(buffer));
			env->DeleteLocalRef(buffer);
		} else {
			_clear_exception(env);
		}
	}

	if (get_audio) {
		jobject buffer = env->CallObjectMethod(object, get_audio);
		if (!env->ExceptionCheck() && buffer) {
			handle->audio_ptr = reinterpret_cast<const float *>(env->GetDirectBufferAddress(buffer));
			env->DeleteLocalRef(buffer);
		} else {
			_clear_exception(env);
		}
	}

	handle->channels = handle->m_get_channels ? env->CallIntMethod(object, handle->m_get_channels) : 0;
	_clear_exception(env);

	return handle;
}

void mp4_decoder_destroy(MP4VideoDecoderHandle *p_handle) {
	if (!p_handle) {
		return;
	}

	JNIEnv *env = get_jni_env();
	if (env && p_handle->object) {
		if (p_handle->m_release) {
			env->CallVoidMethod(p_handle->object, p_handle->m_release);
			_clear_exception(env);
		}
		env->DeleteGlobalRef(p_handle->object);
	}

	memdelete(p_handle);
}

void mp4_decoder_play(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_play) {
		return;
	}
	env->CallVoidMethod(p_handle->object, p_handle->m_play);
	_clear_exception(env);
}

void mp4_decoder_stop(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_stop) {
		return;
	}
	env->CallVoidMethod(p_handle->object, p_handle->m_stop);
	_clear_exception(env);
}

void mp4_decoder_set_paused(MP4VideoDecoderHandle *p_handle, bool p_paused) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_set_paused) {
		return;
	}
	env->CallVoidMethod(p_handle->object, p_handle->m_set_paused, p_paused ? JNI_TRUE : JNI_FALSE);
	_clear_exception(env);
}

bool mp4_decoder_is_playing(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_is_playing) {
		return false;
	}
	jboolean playing = env->CallBooleanMethod(p_handle->object, p_handle->m_is_playing);
	if (env->ExceptionCheck()) {
		_clear_exception(env);
		return false;
	}
	return playing == JNI_TRUE;
}

void mp4_decoder_seek(MP4VideoDecoderHandle *p_handle, double p_seconds) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_seek) {
		return;
	}
	env->CallVoidMethod(p_handle->object, p_handle->m_seek, (jdouble)p_seconds);
	_clear_exception(env);
}

double mp4_decoder_get_duration(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_duration) {
		return 0.0;
	}
	jdouble value = env->CallDoubleMethod(p_handle->object, p_handle->m_get_duration);
	_clear_exception(env);
	return (double)value;
}

double mp4_decoder_get_position(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_position) {
		return 0.0;
	}
	jdouble value = env->CallDoubleMethod(p_handle->object, p_handle->m_get_position);
	_clear_exception(env);
	return (double)value;
}

int mp4_decoder_get_width(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_video_width) {
		return 0;
	}
	jint value = env->CallIntMethod(p_handle->object, p_handle->m_get_video_width);
	_clear_exception(env);
	return (int)value;
}

int mp4_decoder_get_height(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_video_height) {
		return 0;
	}
	jint value = env->CallIntMethod(p_handle->object, p_handle->m_get_video_height);
	_clear_exception(env);
	return (int)value;
}

int mp4_decoder_get_channels(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_channels) {
		return 0;
	}
	jint value = env->CallIntMethod(p_handle->object, p_handle->m_get_channels);
	_clear_exception(env);
	return (int)value;
}

int mp4_decoder_get_sample_rate(MP4VideoDecoderHandle *p_handle) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_get_sample_rate) {
		return 0;
	}
	jint value = env->CallIntMethod(p_handle->object, p_handle->m_get_sample_rate);
	_clear_exception(env);
	return (int)value;
}

bool mp4_decoder_update(MP4VideoDecoderHandle *p_handle, double p_delta) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_update) {
		return false;
	}
	jboolean updated = env->CallBooleanMethod(p_handle->object, p_handle->m_update, (jdouble)p_delta);
	if (env->ExceptionCheck()) {
		_clear_exception(env);
		return false;
	}
	return updated == JNI_TRUE;
}

const uint8_t *mp4_decoder_get_frame(MP4VideoDecoderHandle *p_handle, int *r_width, int *r_height, int *r_rotation, int *r_serial) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->yuv_ptr) {
		return nullptr;
	}

	jint width = p_handle->m_get_frame_width ? env->CallIntMethod(p_handle->object, p_handle->m_get_frame_width) : 0;
	jint height = p_handle->m_get_frame_height ? env->CallIntMethod(p_handle->object, p_handle->m_get_frame_height) : 0;
	jint rotation = p_handle->m_get_rotation ? env->CallIntMethod(p_handle->object, p_handle->m_get_rotation) : 0;
	jint serial = p_handle->m_get_frame_serial ? env->CallIntMethod(p_handle->object, p_handle->m_get_frame_serial) : 0;
	if (env->ExceptionCheck()) {
		_clear_exception(env);
		return nullptr;
	}

	if (r_width) {
		*r_width = (int)width;
	}
	if (r_height) {
		*r_height = (int)height;
	}
	if (r_rotation) {
		*r_rotation = (int)rotation;
	}
	if (r_serial) {
		*r_serial = (int)serial;
	}

	return p_handle->yuv_ptr;
}

int mp4_decoder_mix_audio(MP4VideoDecoderHandle *p_handle, float *p_out, int p_frames) {
	JNIEnv *env = get_jni_env();
	if (!env || !p_handle || !p_handle->m_mix_audio || p_handle->channels <= 0) {
		return 0;
	}

	jint mixed = env->CallIntMethod(p_handle->object, p_handle->m_mix_audio, (jint)p_frames);
	if (env->ExceptionCheck()) {
		_clear_exception(env);
		return 0;
	}
	if (mixed <= 0) {
		return 0;
	}

	if (p_out && p_handle->audio_ptr) {
		memcpy(p_out, p_handle->audio_ptr, (size_t)mixed * (size_t)p_handle->channels * sizeof(float));
	}

	return (int)mixed;
}

#else

struct MP4VideoDecoderHandle {};

MP4VideoDecoderHandle *mp4_decoder_create(const String &p_path, String *r_error) {
	(void)p_path;
	if (r_error) {
		*r_error = "MP4 playback hanya tersedia di Android";
	}
	return nullptr;
}

void mp4_decoder_destroy(MP4VideoDecoderHandle *) {}
void mp4_decoder_play(MP4VideoDecoderHandle *) {}
void mp4_decoder_stop(MP4VideoDecoderHandle *) {}
void mp4_decoder_set_paused(MP4VideoDecoderHandle *, bool) {}
bool mp4_decoder_is_playing(MP4VideoDecoderHandle *) { return false; }
void mp4_decoder_seek(MP4VideoDecoderHandle *, double) {}
double mp4_decoder_get_duration(MP4VideoDecoderHandle *) { return 0.0; }
double mp4_decoder_get_position(MP4VideoDecoderHandle *) { return 0.0; }
int mp4_decoder_get_width(MP4VideoDecoderHandle *) { return 0; }
int mp4_decoder_get_height(MP4VideoDecoderHandle *) { return 0; }
int mp4_decoder_get_channels(MP4VideoDecoderHandle *) { return 0; }
int mp4_decoder_get_sample_rate(MP4VideoDecoderHandle *) { return 0; }
bool mp4_decoder_update(MP4VideoDecoderHandle *, double) { return false; }
const uint8_t *mp4_decoder_get_frame(MP4VideoDecoderHandle *, int *, int *, int *, int *) { return nullptr; }
int mp4_decoder_mix_audio(MP4VideoDecoderHandle *, float *, int) { return 0; }

#endif // ANDROID_ENABLED
