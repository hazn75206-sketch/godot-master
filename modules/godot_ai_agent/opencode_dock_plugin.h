#ifndef GODOT_OPENCODE_DOCK_PLUGIN_H
#define GODOT_OPENCODE_DOCK_PLUGIN_H

#ifdef TOOLS_ENABLED

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"

#include <thread>

// Dock chat replika-TUI untuk agent dalam editor.
// Backend: agent tiruan-opencode (modules/godot_ai_agent/agent_chat).
class OpencodeDockPlugin : public EditorPlugin {
	GDCLASS(OpencodeDockPlugin, EditorPlugin);

	VBoxContainer *dock = nullptr;
	RichTextLabel *output = nullptr;
	LineEdit *input = nullptr;
	Button *send_btn = nullptr;
	Button *cancel_btn = nullptr;
	OptionButton *model_opt = nullptr;
	Button *load_btn = nullptr;
	Label *status = nullptr;
	Label *model_lbl = nullptr;
	String agent_session;
	bool busy = false;

	void _append_log(const String &p_who, const String &p_text, const Color &p_color);
	void _append_badge(const String &p_text);
	bool _handle_slash(const String &p_text);
	String _current_model() const;
	String _current_model_full() const;
	void _on_send();
	void _on_send_text(const String &p_text);
	void _on_cancel();
	void _on_load_models();
	void _on_models(const Dictionary &p_res);
	void _on_model_selected(int p_idx);
	void _on_result(const Dictionary &p_res);
	void _set_busy(bool p_busy);

protected:
	static void _bind_methods();

public:
	virtual String get_plugin_name() const override { return "OpenCode"; }
	virtual void _notification(int p_notification);
	void _enter_plugin();
	void _exit_plugin();
};

#endif // TOOLS_ENABLED

#endif // GODOT_OPENCODE_DOCK_PLUGIN_H
