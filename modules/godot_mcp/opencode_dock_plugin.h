#ifndef GODOT_OPENCODE_DOCK_PLUGIN_H
#define GODOT_OPENCODE_DOCK_PLUGIN_H

#ifdef TOOLS_ENABLED

#include "editor/plugins/editor_plugin.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"

#include <thread>

// Dock chat replika-TUI untuk opencode dalam editor.
// Backend: binari opencode asli via modules/godot_mcp/opencode_runner.
class OpencodeDockPlugin : public EditorPlugin {
	GDCLASS(OpencodeDockPlugin, EditorPlugin);

	VBoxContainer *dock = nullptr;
	RichTextLabel *output = nullptr;
	LineEdit *input = nullptr;
	Button *send_btn = nullptr;
	Label *status = nullptr;
	bool busy = false;

	void _append_log(const String &p_who, const String &p_text, const Color &p_color);
	void _on_send();
	void _on_send_text(const String &p_text);
	void _on_result(const Dictionary &p_res);
	void _set_busy(bool p_busy);

protected:
	static void _bind_methods();

public:
	virtual String _get_plugin_name() const override { return "OpenCode"; }
	virtual void _enter_tree() override;
	virtual void _exit_tree() override;
};

#endif // TOOLS_ENABLED

#endif // GODOT_OPENCODE_DOCK_PLUGIN_H
