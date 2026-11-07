/**************************************************************************/
/*  mp4_preview.cpp                                                       */
/*                                                                        */
/*  Editor preview generator for MP4 videos (first-frame thumbnail).      */
/**************************************************************************/

#include "mp4_preview.h"

#ifdef TOOLS_ENABLED

#include "mp4_android_jni.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/string/ustring.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/video_stream.h"

static const char *MP4_THUMBNAIL_CACHE_DIR = "mp4_video_thumbnails";

static String _thumbnail_cache_base(const String &p_path, const Size2 &p_size) {
	String key = ProjectSettings::get_singleton()->globalize_path(p_path).md5_text();
	key += "_" + itos((int)p_size.x) + "x" + itos((int)p_size.y);
	return "user://" + String(MP4_THUMBNAIL_CACHE_DIR) + "/" + key;
}

Ref<Texture2D> MP4VideoPreviewGenerator::_generate_thumbnail(const String &p_path, const Size2 &p_size) const {
	if (p_path.is_empty() || p_size.x <= 0 || p_size.y <= 0) {
		return Ref<Texture2D>();
	}

	const uint64_t modified_time = FileAccess::get_modified_time(p_path);
	const int64_t file_size = FileAccess::get_size(p_path);
	const bool cacheable = modified_time != 0 && file_size >= 0;

	const String cache_base = _thumbnail_cache_base(p_path, p_size);
	const String image_path = cache_base + ".png";
	const String metadata_path = cache_base + ".txt";

	if (cacheable) {
		Ref<FileAccess> metadata = FileAccess::open(metadata_path, FileAccess::READ);
		if (metadata.is_valid()) {
			const uint64_t cached_modified_time = metadata->get_line().to_int();
			const int64_t cached_file_size = metadata->get_line().to_int();
			metadata->close();

			if (cached_modified_time == modified_time && cached_file_size == file_size) {
				Ref<Image> cached_image;
				cached_image.instantiate();
				if (cached_image->load(image_path) == OK) {
					return ImageTexture::create_from_image(cached_image);
				}
			}
		}
	}

	Vector<uint8_t> rgba;
	int width = 0;
	int height = 0;
	if (!mp4_get_thumbnail(p_path, (int)p_size.x, (int)p_size.y, rgba, width, height)) {
		return Ref<Texture2D>();
	}
	if (width <= 0 || height <= 0 || rgba.size() < (size_t)width * (size_t)height * 4) {
		return Ref<Texture2D>();
	}

	Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, rgba);
	if (image.is_null()) {
		return Ref<Texture2D>();
	}

	if (cacheable) {
		Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_USERDATA);
		if (dir.is_valid()) {
			dir->make_dir_recursive(MP4_THUMBNAIL_CACHE_DIR);
		}

		image->save_png(image_path);

		Ref<FileAccess> metadata = FileAccess::open(metadata_path, FileAccess::WRITE);
		if (metadata.is_valid()) {
			metadata->store_line(String::num_uint64(modified_time));
			metadata->store_line(String::num_int64(file_size));
		}
	}

	return ImageTexture::create_from_image(image);
}

bool MP4VideoPreviewGenerator::handles(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VideoStreamMP4");
}

Ref<Texture2D> MP4VideoPreviewGenerator::generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const {
	(void)p_metadata;

	Ref<VideoStream> stream = p_from;
	if (stream.is_null()) {
		return Ref<Texture2D>();
	}

	return _generate_thumbnail(stream->get_file(), p_size);
}

Ref<Texture2D> MP4VideoPreviewGenerator::generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const {
	(void)p_metadata;

	return _generate_thumbnail(p_path, p_size);
}

bool MP4VideoPreviewGenerator::generate_small_preview_automatically() const {
	return true;
}

#endif // TOOLS_ENABLED
