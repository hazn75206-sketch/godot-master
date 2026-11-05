/**************************************************************************/
/*  register_types.cpp                                                    */
/*                                                                        */
/*  MP4 video playback module for Android (MediaCodec).                   */
/**************************************************************************/

#include "register_types.h"

#include "video_stream_mp4.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"

static Ref<ResourceFormatLoaderMP4> resource_loader_mp4;

void initialize_mp4_video_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	resource_loader_mp4.instantiate();
	ResourceLoader::add_resource_format_loader(resource_loader_mp4, true);
	GDREGISTER_CLASS(VideoStreamMP4);
}

void uninitialize_mp4_video_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ResourceLoader::remove_resource_format_loader(resource_loader_mp4);
	resource_loader_mp4.unref();
}
