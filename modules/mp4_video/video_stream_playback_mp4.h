/**************************************************************************/
/*  video_stream_playback_mp4.h                                           */
/*                                                                        */
/*  MP4 video playback using Android's MediaCodec (JNI bridge).           */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "scene/resources/video_stream.h"

class ImageTexture;
struct MP4VideoDecoderHandle;

class VideoStreamPlaybackMP4 : public VideoStreamPlayback {
	MP4VideoDecoderHandle *decoder = nullptr;

	Ref<ImageTexture> texture;
	Ref<Image> image;

	Vector<float> audio_scratch;
	Vector<float> audio_read_scratch;
	Vector<float> audio_pending;

	String file;
	bool valid = false;

	int channels = 0;
	int mix_rate = 0;
	double length = 0.0;

	int texture_width = 0;
	int texture_height = 0;
	int uploaded_serial = -1;

	uint64_t file_modified_time = 0;
	int64_t file_size = 0;

	// Decoding happens on a worker thread so the main thread only uploads the
	// latest converted frame and feeds audio into Godot's resampler.
	Thread decode_thread;
	bool thread_started = false;
	SafeFlag exit_thread;
	SafeFlag playing;
	SafeFlag paused_flag;
	SafeNumeric<float> position;
	Mutex decoder_mutex; // Serializes calls into the Java decoder.

	Mutex frame_mutex;
	Vector<uint8_t> frame_front;
	Vector<uint8_t> frame_back;
	int frame_width = 0;
	int frame_height = 0;
	int frame_serial = 0;

	Mutex audio_mutex;
	Vector<float> audio_ring;
	int audio_read = 0;
	int audio_write = 0;
	int audio_count = 0;

	void _open();
	void _close();
	void _start_thread();
	static void _thread_callback(void *p_user);
	void _thread_loop();
	void _convert_frame(const uint8_t *p_yuv, int p_width, int p_height, int p_rotation);
	void _upload_frame(const Vector<uint8_t> &p_rgba, int p_width, int p_height);
	void _push_audio(const float *p_data, int p_frames);
	int _pop_audio(float *p_out, int p_max_frames);
	int _queued_audio_frames();
	void _clear_audio();
	void _clear_frames();

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
