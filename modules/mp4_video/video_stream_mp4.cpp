/**************************************************************************/
/*  video_stream_mp4.cpp                                                  */
/*                                                                        */
/*  MP4 VideoStream resource and loader.                                  */
/**************************************************************************/

#include "video_stream_mp4.h"

#include "video_stream_playback_mp4.h"

#include "core/io/file_access.h"
#include "core/object/class_db.h"

void VideoStreamMP4::_bind_methods() {
}

Ref<VideoStreamPlayback> VideoStreamMP4::instantiate_playback() {
	Ref<VideoStreamPlaybackMP4> playback = memnew(VideoStreamPlaybackMP4);
	playback->set_audio_track(audio_track);
	playback->set_file(file);
	return playback;
}

void VideoStreamMP4::reload_from_file() {
	Resource::reload_from_file();
	// The resource only stores the path, so the reloaded copy is identical.
	// Emit `changed` anyway so VideoStreamPlayer recreates its playback and
	// the new file contents are picked up.
	emit_changed();
}

Ref<Resource> ResourceFormatLoaderMP4::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null()) {
		if (r_error) {
			*r_error = ERR_CANT_OPEN;
		}
		return Ref<Resource>();
	}
	f.unref();

	Ref<VideoStreamMP4> stream;
	stream.instantiate();
	stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}

	return stream;
}

void ResourceFormatLoaderMP4::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("mp4");
	p_extensions->push_back("m4v");
}

bool ResourceFormatLoaderMP4::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VideoStream");
}

String ResourceFormatLoaderMP4::get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "mp4" || p_path.get_extension().to_lower() == "m4v") {
		return "VideoStreamMP4";
	}
	return "";
}
