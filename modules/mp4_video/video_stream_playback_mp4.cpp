/**************************************************************************/
/*  video_stream_playback_mp4.cpp                                         */
/*                                                                        */
/*  MP4 video playback using Android's MediaCodec (JNI bridge).           */
/**************************************************************************/

#include "video_stream_playback_mp4.h"

#include "mp4_android_jni.h"

#include "core/error/error_macros.h"
#include "core/io/image.h"
#include "core/string/ustring.h"
#include "scene/resources/image_texture.h"

#include <cstring>

VideoStreamPlaybackMP4::VideoStreamPlaybackMP4() {
	texture.instantiate();
}

VideoStreamPlaybackMP4::~VideoStreamPlaybackMP4() {
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
	position = 0.0;

	if (channels <= 0 || mix_rate <= 0) {
		channels = 0;
		mix_rate = 0;
	}
}

void VideoStreamPlaybackMP4::_update_texture(const uint8_t *p_yuv, int p_width, int p_height, int p_rotation) {
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

	rgba_frame.resize(out_w * out_h * 4);
	uint8_t *dst = rgba_frame.ptrw();

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

	if (texture_width != out_w || texture_height != out_h) {
		image = Image::create_from_data(out_w, out_h, false, Image::FORMAT_RGBA8, rgba_frame);
		texture->set_image(image);
		texture_width = out_w;
		texture_height = out_h;
	} else {
		image->set_data(out_w, out_h, false, Image::FORMAT_RGBA8, rgba_frame);
		texture->update(image);
	}
}

void VideoStreamPlaybackMP4::stop() {
	if (decoder) {
		mp4_decoder_stop(decoder);
	}
	playing = false;
	paused = false;
	position = 0.0;
	audio_pending.clear();
	last_serial = -1;
}

void VideoStreamPlaybackMP4::play() {
	if (!valid || !decoder) {
		return;
	}
	mp4_decoder_play(decoder);
	playing = true;
	paused = false;
	audio_pending.clear();
	last_serial = -1;
}

bool VideoStreamPlaybackMP4::is_playing() const {
	return valid && playing;
}

void VideoStreamPlaybackMP4::set_paused(bool p_paused) {
	paused = p_paused;
	if (decoder) {
		mp4_decoder_set_paused(decoder, p_paused);
	}
}

bool VideoStreamPlaybackMP4::is_paused() const {
	return paused;
}

double VideoStreamPlaybackMP4::get_length() const {
	return length;
}

double VideoStreamPlaybackMP4::get_playback_position() const {
	return position;
}

void VideoStreamPlaybackMP4::seek(double p_time) {
	if (!valid || !decoder) {
		return;
	}
	mp4_decoder_seek(decoder, p_time);
	position = p_time;
	audio_pending.clear();
	last_serial = -1;
}

void VideoStreamPlaybackMP4::set_audio_track(int p_idx) {
	(void)p_idx;
	// Only the default audio track is supported.
}

Ref<Texture2D> VideoStreamPlaybackMP4::get_texture() const {
	return texture;
}

void VideoStreamPlaybackMP4::update(double p_delta) {
	if (!valid || !decoder || !playing || paused) {
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
			int frames = (int)(p_delta * mix_rate);
			if (frames > 0) {
				audio_scratch.resize(frames * channels);
				const int got = mp4_decoder_mix_audio(decoder, audio_scratch.ptrw(), frames);
				if (got > 0) {
					const int accepted = mix_callback(mix_udata, audio_scratch.ptr(), got);
					if (accepted < got) {
						const int remaining = (got - accepted) * channels;
						audio_pending.resize(remaining);
						memcpy(audio_pending.ptrw(), audio_scratch.ptr() + accepted * channels, remaining * sizeof(float));
					}
				}
			}
		}
	}

	if (mp4_decoder_update(decoder, p_delta)) {
		int width = 0;
		int height = 0;
		int rotation = 0;
		int serial = 0;
		const uint8_t *yuv = mp4_decoder_get_frame(decoder, &width, &height, &rotation, &serial);
		if (yuv && serial != last_serial) {
			last_serial = serial;
			_update_texture(yuv, width, height, rotation);
		}
	}

	position = mp4_decoder_get_position(decoder);

	if (!mp4_decoder_is_playing(decoder)) {
		playing = false;
	}
}

int VideoStreamPlaybackMP4::get_channels() const {
	return channels;
}

int VideoStreamPlaybackMP4::get_mix_rate() const {
	return mix_rate;
}
