/**************************************************************************/
/*  boot_splash_player.h                                                  */
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

#pragma once

#include "scene/gui/control.h"
#include "scene/resources/texture.h"

class BootSplashPlayer : public Control {
	GDCLASS(BootSplashPlayer, Control);

public:
	enum Preset {
		PRESET_NONE,
		PRESET_DEMO,
	};

private:
	Ref<Texture2D> texture;
	Color background_color = Color(0.14f, 0.14f, 0.14f);
	Preset preset = PRESET_NONE;
	float duration = 4.0f;
	float hold = 2.3f;
	float elapsed = 0.0f;
	bool resume_tree = true;
	bool finished = false;

	void _finish();

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _draw();

public:
	void setup(const Ref<Image> &p_image, const Color &p_background, Preset p_preset, float p_duration, float p_hold);
	void set_resume_tree(bool p_resume);

	BootSplashPlayer();
};
