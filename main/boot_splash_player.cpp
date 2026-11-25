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

static float _smooth_progress(float p_value) {
	float t = CLAMP(p_value, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void BootSplashPlayer::_bind_methods() {
}

void BootSplashPlayer::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		queue_redraw();
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

void BootSplashPlayer::_ready() {
	set_anchors_and_offsets_preset(PRESET_FULL_RECT);
	set_process_mode(PROCESS_MODE_ALWAYS);
	set_process(true);
}

void BootSplashPlayer::_process(double p_delta) {
	if (finished) {
		return;
	}

	elapsed += (float)p_delta;
	// Unity plays a full fade-in/hold/fade-out cycle, so it lasts longer than the exit-only presets.
	const float total_time = (preset == PRESET_UNITY) ? (duration * 2.0f + hold) : (duration + hold);
	if (elapsed >= total_time) {
		_finish();
		return;
	}

	queue_redraw();
}

void BootSplashPlayer::_draw() {
	Size2 view_size = get_viewport_rect().size;
	if (view_size.x <= 0.0f || view_size.y <= 0.0f) {
		return;
	}

	draw_rect(Rect2(Vector2(), view_size), background_color);

	if (texture.is_null()) {
		return;
	}

	Size2 image_size = texture->get_size();
	if (image_size.x <= 0.0f || image_size.y <= 0.0f) {
		return;
	}

	// Unity preset: fade the loading image in, keep it visible during load, then fade it out.
	if (preset == PRESET_UNITY) {
		const float fade_time = MAX(duration, 0.001f);
		float unity_alpha = 1.0f;
		if (elapsed < fade_time) {
			unity_alpha = _smooth_progress(elapsed / fade_time);
		} else if (elapsed < fade_time + hold) {
			unity_alpha = 1.0f;
		} else {
			unity_alpha = 1.0f - _smooth_progress((elapsed - fade_time - hold) / fade_time);
		}

		float unity_fit = MIN(view_size.x / image_size.x, view_size.y / image_size.y);
		Size2 unity_draw_size = image_size * unity_fit;
		Vector2 unity_center = view_size * 0.5f;
		draw_texture_rect(texture, Rect2(unity_center - unity_draw_size * 0.5f, unity_draw_size), false, Color(1.0f, 1.0f, 1.0f, unity_alpha));
		return;
	}

	// Exit-only animation: static boot image is already visible, we animate the exit transition.
	float out_time = MAX(duration, 0.001f);
	float out_start = hold;

	float exit = (elapsed > out_start) ? _smooth_progress((elapsed - out_start) / out_time) : 0.0f;

	float alpha = 1.0f;
	float scale = 1.0f;
	Vector2 offset;
	float angle = 0.0f;

	switch (preset) {
		case PRESET_FADE: {
			alpha = 1.0f - exit;
		} break;
		case PRESET_ZOOM_IN: {
			scale = Math::lerp(1.0f, 1.15f, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_ZOOM_OUT: {
			scale = Math::lerp(1.0f, 0.85f, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_SLIDE_LEFT: {
			offset.x = Math::lerp(0.0f, -view_size.x, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_SLIDE_RIGHT: {
			offset.x = Math::lerp(0.0f, view_size.x, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_SLIDE_UP: {
			offset.y = Math::lerp(0.0f, -view_size.y, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_SLIDE_DOWN: {
			offset.y = Math::lerp(0.0f, view_size.y, exit);
			alpha = 1.0f - exit;
		} break;
		case PRESET_PULSE: {
			scale = 1.0f + 0.06f * Math::sin((elapsed / MAX(duration, 0.001f)) * (float)Math::TAU * 2.0f);
			alpha = 1.0f - exit;
		} break;
		case PRESET_ROTATE: {
			angle = Math::lerp(0.0f, (float)Math::PI / 6.0f, exit);
			scale = 1.0f;
			alpha = 1.0f - exit;
		} break;
		default: {
			alpha = 1.0f - exit;
		} break;
	}

	float fit = MIN(view_size.x / image_size.x, view_size.y / image_size.y);
	Size2 draw_size = image_size * fit * scale;
	Vector2 center = view_size * 0.5f + offset;

	draw_set_transform(center, angle, Vector2(1.0f, 1.0f));
	draw_texture_rect(texture, Rect2(-draw_size * 0.5f, draw_size), false, Color(1.0f, 1.0f, 1.0f, alpha));
	draw_set_transform(Vector2(), 0.0f, Vector2(1.0f, 1.0f));
}

BootSplashPlayer::BootSplashPlayer() {
	set_mouse_filter(MOUSE_FILTER_STOP);
	set_process_mode(PROCESS_MODE_ALWAYS);
}
