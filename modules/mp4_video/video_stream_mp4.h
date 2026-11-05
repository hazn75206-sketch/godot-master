/**************************************************************************/
/*  video_stream_mp4.h                                                    */
/*                                                                        */
/*  MP4 VideoStream resource and loader.                                  */
/**************************************************************************/

#pragma once

#include "core/io/resource_loader.h"
#include "scene/resources/video_stream.h"

class VideoStreamMP4 : public VideoStream {
	GDCLASS(VideoStreamMP4, VideoStream);

protected:
	static void _bind_methods();

public:
	virtual Ref<VideoStreamPlayback> instantiate_playback() override;
	virtual void set_audio_track(int p_track) override { audio_track = p_track; }
};

class ResourceFormatLoaderMP4 : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderMP4, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
