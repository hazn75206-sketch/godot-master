/**************************************************************************/
/*  boot_splash_player.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "boot_splash_player.h"

#include "core/math/math_funcs.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/image_texture.h"

static float _cubic_ease_in_out(float p_value) {
	float t = CLAMP(p_value, 0.0f, 1.0f);
	return t < 0.5f ? 4.0f * t * t * t : 1.0f - Math::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

static float _quad_ease_in_out(float p_value) {
	float t = CLAMP(p_value, 0.0f, 1.0f);
	return t < 0.5f ? 2.0f * t * t : 1.0f - Math::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
}

void BootSplashPlayer::_bind_methods() {
}

void BootSplashPlayer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_anchors_and_offsets_preset(PRESET_FULL_RECT);
			set_process(true);
		} break;
		case NOTIFICATION_PROCESS: {
			if (finished) {
				break;
			}
			elapsed += (float)get_process_delta_time();
			if (elapsed >= duration) {
				_finish();
			} else {
				queue_redraw();
			}
		} break;
		case NOTIFICATION_DRAW: {
			_draw();
		} break;
		case NOTIFICATION_RESIZED: {
			queue_redraw();
		} break;
	}
}

void BootSplashPlayer::_finish() {
	if (finished) {
		return;
	}
	finished = true;
	set_process(false);

	if (resume_tree) {
		SceneTree *tree = get_tree();
		if (tree) {
			tree->set_pause(false);
		}
	}

	Node *parent = get_parent();
	if (parent) {
		parent->queue_free();
	} else {
		queue_free();
	}
}

void BootSplashPlayer::setup(const Ref<Image> &p_image, const Color &p_background, Preset p_preset, float p_duration, float p_hold) {
	texture = ImageTexture::create_from_image(p_image);
	background_color = p_background;
	preset = p_preset;
	duration = MAX(p_duration, 0.01f);
	hold = MAX(p_hold, 0.0f);
	elapsed = 0.0f;
	finished = false;
	set_process(true);
	queue_redraw();
}

void BootSplashPlayer::set_resume_tree(bool p_resume) {
	resume_tree = p_resume;
}

void BootSplashPlayer::_draw() {
	Size2 view_size = get_viewport_rect().size;
	if (view_size.x <= 0.0f || view_size.y <= 0.0f) {
		return;
	}

	draw_rect(Rect2(Vector2(), view_size), background_color);

	if (preset != PRESET_DEMO || texture.is_null()) {
		return;
	}

	Size2 image_size = texture->get_size();
	if (image_size.x <= 0.0f || image_size.y <= 0.0f) {
		return;
	}

	// Mirror the demo's AnimationPlayer timing: fade in (0.7s), hold (2.3s), fade out (1.0s)
	// with a 4.0s total. The ratios are preserved when the duration/hold are customized.
	const float total_time = MAX(duration, 0.01f);
	const float hold_time = CLAMP(hold, 0.0f, MAX(total_time - 0.01f, 0.0f));
	const float anim_time = MAX(total_time - hold_time, 0.001f);
	const float fade_in = anim_time * (0.7f / 1.7f);
	const float fade_out = anim_time * (1.0f / 1.7f);

	// The demo moves the logo by 30px on a 648px tall viewport.
	const float rise = view_size.y * (30.0f / 648.0f);

	float overlay_alpha = 0.0f;
	float offset_y = 0.0f;

	if (elapsed < fade_in) {
		const float p = elapsed / fade_in;
		overlay_alpha = 1.0f - _cubic_ease_in_out(p);
		offset_y = Math::lerp(rise, 0.0f, _quad_ease_in_out(p));
	} else if (elapsed < fade_in + hold_time) {
		overlay_alpha = 0.0f;
		offset_y = 0.0f;
	} else {
		const float p = CLAMP((elapsed - fade_in - hold_time) / MAX(fade_out, 0.001f), 0.0f, 1.0f);
		overlay_alpha = _cubic_ease_in_out(p);
		offset_y = Math::lerp(0.0f, -rise, _quad_ease_in_out(p));
	}

	const float fit = MIN(view_size.x / image_size.x, view_size.y / image_size.y);
	const Size2 draw_size = image_size * fit;
	const Vector2 center = view_size * 0.5f + Vector2(0.0f, offset_y);
	draw_texture_rect(texture, Rect2(center - draw_size * 0.5f, draw_size), false);

	// The overlay uses the background color so the fade blends into the static boot splash.
	draw_rect(Rect2(Vector2(), view_size), Color(background_color.r, background_color.g, background_color.b, overlay_alpha));
}

BootSplashPlayer::BootSplashPlayer() {
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_process_mode(PROCESS_MODE_ALWAYS);
}
