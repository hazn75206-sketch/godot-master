/**************************************************************************/
/*  video_stream_playback_mp4.cpp                                         */
/*                                                                        */
/*  MP4 video playback using Android's MediaCodec (JNI bridge).           */
/**************************************************************************/

#include "video_stream_playback_mp4.h"

#include "mp4_android_jni.h"

#include "core/error/error_macros.h"
#include "core/io/image.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "scene/resources/image_texture.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <thirdparty/misc/yuv2rgb.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <cstring>

VideoStreamPlaybackMP4::VideoStreamPlaybackMP4() {
	texture.instantiate();
}

VideoStreamPlaybackMP4::~VideoStreamPlaybackMP4() {
	exit_thread.set();
	if (thread_started) {
		decode_thread.wait_to_finish();
		thread_started = false;
	}

	_clear_audio();
	_clear_frames();

	mp4_decoder_destroy(decoder);
	decoder = nullptr;
}

void VideoStreamPlaybackMP4::set_file(const String &p_file) {
	file = p_file;
	_open();
}

void VideoStreamPlaybackMP4::_open() {
	if (file.is_empty() || decoder) {
		return;
	}

	String error;
	decoder = mp4_decoder_create(file, &error);
	if (!decoder) {
		if (!error.is_empty()) {
			ERR_PRINT(vformat("VideoStreamMP4: %s (%s)", error, file));
		}
		return;
	}

	valid = true;
	channels = mp4_decoder_get_channels(decoder);
	mix_rate = mp4_decoder_get_sample_rate(decoder);
	length = mp4_decoder_get_duration(decoder);
	position.set(0.0f);

	if (channels <= 0 || mix_rate <= 0) {
		channels = 0;
		mix_rate = 0;
	} else {
		audio_ring.resize(mix_rate * channels * 2);
		_clear_audio();
	}
}

void VideoStreamPlaybackMP4::_start_thread() {
	if (thread_started) {
		return;
	}

	exit_thread.clear();
	thread_started = decode_thread.start(VideoStreamPlaybackMP4::_thread_callback, this) != Thread::UNASSIGNED_ID;
	if (!thread_started) {
		ERR_PRINT("VideoStreamMP4: gagal membuat decode thread.");
	}
}

void VideoStreamPlaybackMP4::_thread_callback(void *p_user) {
	static_cast<VideoStreamPlaybackMP4 *>(p_user)->_thread_loop();
}

void VideoStreamPlaybackMP4::_thread_loop() {
	OS *os = OS::get_singleton();
	uint64_t last_usec = os->get_ticks_usec();

	while (!exit_thread.is_set()) {
		if (!playing.is_set() || paused_flag.is_set()) {
			last_usec = os->get_ticks_usec();
			os->delay_usec(3000);
			continue;
		}

		const uint64_t now = os->get_ticks_usec();
		const double delta = MIN(double(now - last_usec) / 1000000.0, 0.1);
		last_usec = now;

		{
			MutexLock lock(decoder_mutex);
			if (playing.is_set() && !paused_flag.is_set()) {
				const bool new_frame = mp4_decoder_update(decoder, delta);
				if (new_frame) {
					int frame_w = 0;
					int frame_h = 0;
					int rotation = 0;
					const uint8_t *yuv = mp4_decoder_get_frame(decoder, &frame_w, &frame_h, &rotation, nullptr);
					if (yuv) {
						_convert_frame(yuv, frame_w, frame_h, rotation);
					}
				}

				if (channels > 0 && mix_rate > 0) {
					// Keep roughly 50 ms of decoded PCM queued for the main
					// thread, which feeds it into the engine's resampler.
					const int target_frames = MAX(mix_rate / 20, 1);
					const int queued_frames = _queued_audio_frames();
					int want = target_frames - queued_frames;
					if (want > 0) {
						want = MIN(want, mix_rate / 4);
						audio_scratch.resize(want * channels);
						const int got = mp4_decoder_mix_audio(decoder, audio_scratch.ptrw(), want);
						if (got > 0) {
							_push_audio(audio_scratch.ptr(), got);
						}
					}
				}

				position.set((float)mp4_decoder_get_position(decoder));

				if (!mp4_decoder_is_playing(decoder)) {
					playing.clear();
				}
			}
		}

		os->delay_usec(2000);
	}
}

void VideoStreamPlaybackMP4::_convert_frame(const uint8_t *p_yuv, int p_width, int p_height, int p_rotation) {
	if (!p_yuv || p_width <= 0 || p_height <= 0) {
		return;
	}

	int rotation = p_rotation % 360;
	if (rotation < 0) {
		rotation += 360;
	}

	const int cw = p_width;
	const int ch = p_height;
	const int ccw = (cw + 1) / 2;
	const int cch = (ch + 1) / 2;

	const uint8_t *y_plane = p_yuv;
	const uint8_t *u_plane = y_plane + cw * ch;
	const uint8_t *v_plane = u_plane + ccw * cch;

	const bool swap = (rotation == 90 || rotation == 270);
	const int out_w = swap ? ch : cw;
	const int out_h = swap ? cw : ch;

	frame_back.resize(out_w * out_h * 4);
	uint8_t *dst = frame_back.ptrw();

	if (rotation == 0) {
		yuv420_2_rgb8888(dst, y_plane, u_plane, v_plane, cw, ch, cw, ccw, cw * 4);
	} else {
		for (int y = 0; y < ch; y++) {
			for (int x = 0; x < cw; x++) {
				const int yy = y_plane[y * cw + x];
				const int uu = u_plane[(y / 2) * ccw + (x / 2)] - 128;
				const int vv = v_plane[(y / 2) * ccw + (x / 2)] - 128;

				const int c = yy - 16;
				int r = (298 * c + 409 * vv + 128) >> 8;
				int g = (298 * c - 100 * uu - 208 * vv + 128) >> 8;
				int b = (298 * c + 516 * uu + 128) >> 8;

				r = CLAMP(r, 0, 255);
				g = CLAMP(g, 0, 255);
				b = CLAMP(b, 0, 255);

				int dx = x;
				int dy = y;
				switch (rotation) {
					case 90:
						dx = ch - 1 - y;
						dy = x;
						break;
					case 180:
						dx = cw - 1 - x;
						dy = ch - 1 - y;
						break;
					case 270:
						dx = y;
						dy = cw - 1 - x;
						break;
					default:
						break;
				}

				uint8_t *pixel = dst + ((size_t)dy * (size_t)out_w + (size_t)dx) * 4;
				pixel[0] = (uint8_t)r;
				pixel[1] = (uint8_t)g;
				pixel[2] = (uint8_t)b;
				pixel[3] = 255;
			}
		}
	}

	MutexLock lock(frame_mutex);
	SWAP(frame_front, frame_back);
	frame_width = out_w;
	frame_height = out_h;
	frame_serial++;
}

void VideoStreamPlaybackMP4::_upload_frame(const Vector<uint8_t> &p_rgba, int p_width, int p_height) {
	if (texture_width != p_width || texture_height != p_height) {
		image = Image::create_from_data(p_width, p_height, false, Image::FORMAT_RGBA8, p_rgba);
		texture->set_image(image);
		texture_width = p_width;
		texture_height = p_height;
	} else {
		image->set_data(p_width, p_height, false, Image::FORMAT_RGBA8, p_rgba);
		texture->update(image);
	}
}

void VideoStreamPlaybackMP4::_push_audio(const float *p_data, int p_frames) {
	if (!p_data || p_frames <= 0 || channels <= 0) {
		return;
	}

	MutexLock lock(audio_mutex);
	const int capacity = audio_ring.size();
	if (capacity <= 0) {
		return;
	}

	const int samples = p_frames * channels;
	float *ring = audio_ring.ptrw();
	for (int i = 0; i < samples; i++) {
		if (audio_count >= capacity) {
			break;
		}
		ring[audio_write] = p_data[i];
		audio_write = (audio_write + 1) % capacity;
		audio_count++;
	}
}

int VideoStreamPlaybackMP4::_pop_audio(float *p_out, int p_max_frames) {
	MutexLock lock(audio_mutex);
	if (channels <= 0 || audio_count <= 0) {
		return 0;
	}

	const int available = audio_count / channels;
	const int frames = MIN(p_max_frames, available);
	if (frames <= 0) {
		return 0;
	}

	const int capacity = audio_ring.size();
	if (capacity <= 0) {
		return 0;
	}
	const float *ring = audio_ring.ptr();
	const int samples = frames * channels;
	for (int i = 0; i < samples; i++) {
		p_out[i] = ring[audio_read];
		audio_read = (audio_read + 1) % capacity;
		audio_count--;
	}

	return frames;
}

int VideoStreamPlaybackMP4::_queued_audio_frames() {
	MutexLock lock(audio_mutex);
	return channels > 0 ? audio_count / channels : 0;
}

void VideoStreamPlaybackMP4::_clear_audio() {
	MutexLock lock(audio_mutex);
	audio_read = 0;
	audio_write = 0;
	audio_count = 0;
}

void VideoStreamPlaybackMP4::_clear_frames() {
	MutexLock lock(frame_mutex);
	frame_front.clear();
	frame_back.clear();
	frame_width = 0;
	frame_height = 0;
	frame_serial = 0;
	uploaded_serial = -1;
}

void VideoStreamPlaybackMP4::stop() {
	playing.clear();
	paused_flag.clear();

	if (decoder) {
		MutexLock lock(decoder_mutex);
		mp4_decoder_stop(decoder);
		_clear_audio();
		_clear_frames();
	}

	audio_pending.clear();
	position.set(0.0f);
}

void VideoStreamPlaybackMP4::play() {
	if (!valid || !decoder || playing.is_set()) {
		return;
	}

	{
		MutexLock lock(decoder_mutex);
		_clear_audio();
		_clear_frames();
		mp4_decoder_play(decoder);
		position.set(0.0f);
	}

	audio_pending.clear();
	paused_flag.clear();
	playing.set();
	_start_thread();
}

bool VideoStreamPlaybackMP4::is_playing() const {
	return valid && playing.is_set();
}

void VideoStreamPlaybackMP4::set_paused(bool p_paused) {
	paused_flag.set_to(p_paused);

	if (decoder) {
		MutexLock lock(decoder_mutex);
		mp4_decoder_set_paused(decoder, p_paused);
	}
}

bool VideoStreamPlaybackMP4::is_paused() const {
	return paused_flag.is_set();
}

double VideoStreamPlaybackMP4::get_length() const {
	return length;
}

double VideoStreamPlaybackMP4::get_playback_position() const {
	return (double)position.get();
}

void VideoStreamPlaybackMP4::seek(double p_time) {
	if (!valid || !decoder) {
		return;
	}

	MutexLock lock(decoder_mutex);
	mp4_decoder_seek(decoder, p_time);
	_clear_audio();
	_clear_frames();
	audio_pending.clear();
	position.set((float)p_time);
}

void VideoStreamPlaybackMP4::set_audio_track(int p_idx) {
	(void)p_idx;
	// Only the default audio track is supported.
}

Ref<Texture2D> VideoStreamPlaybackMP4::get_texture() const {
	return texture;
}

void VideoStreamPlaybackMP4::update(double p_delta) {
	if (!valid || !decoder) {
		return;
	}

	if (channels > 0 && mix_rate > 0 && mix_callback) {
		if (!audio_pending.is_empty()) {
			const int pending_frames = audio_pending.size() / channels;
			const int accepted = mix_callback(mix_udata, audio_pending.ptr(), pending_frames);
			if (accepted >= pending_frames) {
				audio_pending.clear();
			} else if (accepted > 0) {
				const int remaining = (pending_frames - accepted) * channels;
				float *pending_ptr = audio_pending.ptrw();
				memmove(pending_ptr, pending_ptr + accepted * channels, remaining * sizeof(float));
				audio_pending.resize(remaining);
			}
		}

		if (audio_pending.is_empty()) {
			const int frames = (int)(p_delta * mix_rate);
			if (frames > 0) {
				audio_read_scratch.resize(frames * channels);
				const int got = _pop_audio(audio_read_scratch.ptrw(), frames);
				if (got > 0) {
					const int accepted = mix_callback(mix_udata, audio_read_scratch.ptr(), got);
					if (accepted < got) {
						const int remaining = (got - accepted) * channels;
						audio_pending.resize(remaining);
						memcpy(audio_pending.ptrw(), audio_read_scratch.ptr() + accepted * channels, remaining * sizeof(float));
					}
				}
			}
		}
	}

	MutexLock lock(frame_mutex);
	if (frame_serial != uploaded_serial && frame_width > 0 && frame_height > 0) {
		_upload_frame(frame_front, frame_width, frame_height);
		uploaded_serial = frame_serial;
	}
}

int VideoStreamPlaybackMP4::get_channels() const {
	return channels;
}

int VideoStreamPlaybackMP4::get_mix_rate() const {
	return mix_rate;
}
