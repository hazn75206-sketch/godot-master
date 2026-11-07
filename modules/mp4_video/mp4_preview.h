/**************************************************************************/
/*  mp4_preview.h                                                         */
/*                                                                        */
/*  Editor preview generator for MP4 videos (first-frame thumbnail).      */
/**************************************************************************/

#pragma once

#ifdef TOOLS_ENABLED

#include "editor/inspector/editor_resource_preview.h"

class MP4VideoPreviewGenerator : public EditorResourcePreviewGenerator {
	GDCLASS(MP4VideoPreviewGenerator, EditorResourcePreviewGenerator);

	Ref<Texture2D> _generate_thumbnail(const String &p_path, const Size2 &p_size) const;

public:
	virtual bool handles(const String &p_type) const override;
	virtual Ref<Texture2D> generate(const Ref<Resource> &p_from, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual Ref<Texture2D> generate_from_path(const String &p_path, const Size2 &p_size, Dictionary &p_metadata) const override;
	virtual bool generate_small_preview_automatically() const override;
};

#endif // TOOLS_ENABLED
