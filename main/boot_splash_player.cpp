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

#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/packed_scene.h"

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

			if (!scene_path.is_empty() && !load_finished) {
				if (!load_requested) {
					Error err = ResourceLoader::load_threaded_request(scene_path);
					load_requested = true;
					if (err != OK) {
						ERR_PRINT("Failed to start loading the main scene: " + scene_path);
						load_finished = true;
					}
				}

				if (!load_finished) {
					const ResourceLoader::ThreadLoadStatus status = ResourceLoader::load_threaded_get_status(scene_path);
					if (status == ResourceLoader::THREAD_LOAD_LOADED) {
						pending_scene_resource = ResourceLoader::load_threaded_get(scene_path);
						load_finished = true;
						if (pending_scene_resource.is_null()) {
							ERR_PRINT("Failed to retrieve the main scene: " + scene_path);
						}
					} else if (status == ResourceLoader::THREAD_LOAD_FAILED || status == ResourceLoader::THREAD_LOAD_INVALID_RESOURCE) {
						ERR_PRINT("Failed to load the main scene: " + scene_path);
						load_finished = true;
					}
				}
			}

			const bool scene_ready = scene_path.is_empty() || load_finished;

			const float total_time = MAX(duration, 0.01f);
			const float hold_time = CLAMP(hold, 0.0f, MAX(total_time - 0.01f, 0.0f));
			const float anim_time = MAX(total_time - hold_time, 0.001f);
			const float fade_in = anim_time * (0.7f / 1.7f);
			const float fade_out = anim_time * (1.0f / 1.7f);

			// Keep the logo on screen until the main scene is ready, then fade out.
			if (elapsed >= fade_in + hold_time && scene_ready) {
				if (fade_out_start < 0.0f) {
					fade_out_start = elapsed;
				}
				if (elapsed - fade_out_start >= fade_out) {
					_finish();
					break;
				}
			}

			queue_redraw();
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

	SceneTree *tree = get_tree();
	if (!scene_path.is_empty()) {
		PackedScene *packed_scene = Object::cast_to<PackedScene>(pending_scene_resource.ptr());
		Node *scene = packed_scene ? packed_scene->instantiate() : nullptr;
		if (scene && tree) {
			tree->add_current_scene(scene);
		} else {
			ERR_PRINT("Failed to instantiate the main scene: " + scene_path);
			if (tree) {
				tree->quit();
			}
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
	fade_out_start = -1.0f;
	pending_scene_resource.unref();
	load_requested = false;
	load_finished = false;
	finished = false;
	set_process(true);
	queue_redraw();
}

void BootSplashPlayer::set_scene_path(const String &p_path) {
	scene_path = p_path;
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
	} else if (fade_out_start < 0.0f) {
		// Hold the logo while waiting for the main scene to finish loading.
		overlay_alpha = 0.0f;
		offset_y = 0.0f;
	} else {
		const float p = CLAMP((elapsed - fade_out_start) / MAX(fade_out, 0.001f), 0.0f, 1.0f);
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
