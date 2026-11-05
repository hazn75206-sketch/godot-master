/**************************************************************************/
/*  video_stream_playback_mp4.h                                           */
/*                                                                        */
/*  MP4 video playback using Android's MediaCodec (JNI bridge).           */
/**************************************************************************/

#pragma once

#include "scene/resources/video_stream.h"

class ImageTexture;
struct MP4VideoDecoderHandle;

class VideoStreamPlaybackMP4 : public VideoStreamPlayback {
	MP4VideoDecoderHandle *decoder = nullptr;

	Ref<ImageTexture> texture;
	Ref<Image> image;

	Vector<uint8_t> rgba_frame;
	Vector<float> audio_scratch;
	Vector<float> audio_pending;

	String file;
	bool valid = false;
	bool playing = false;
	bool paused = false;

	int channels = 0;
	int mix_rate = 0;
	double length = 0.0;
	double position = 0.0;

	int texture_width = 0;
	int texture_height = 0;
	int last_serial = -1;

	void _open();
	void _update_texture(const uint8_t *p_yuv, int p_width, int p_height, int p_rotation);

public:
	VideoStreamPlaybackMP4();
	~VideoStreamPlaybackMP4();

	void set_file(const String &p_file);

	virtual void stop() override;
	virtual void play() override;
	virtual bool is_playing() const override;
	virtual void set_paused(bool p_paused) override;
	virtual bool is_paused() const override;

	virtual double get_length() const override;
	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;

	virtual void set_audio_track(int p_idx) override;

	virtual Ref<Texture2D> get_texture() const override;
	virtual void update(double p_delta) override;

	virtual int get_channels() const override;
	virtual int get_mix_rate() const override;
};
