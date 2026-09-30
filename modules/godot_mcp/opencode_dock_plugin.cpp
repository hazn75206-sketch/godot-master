#include "opencode_dock_plugin.h"

#ifdef TOOLS_ENABLED

#include "agent_chat.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/variant/dictionary.h"
#include "editor/settings/editor_settings.h"

#include <thread>

void OpencodeDockPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_result", "res"), &OpencodeDockPlugin::_on_result);
	ClassDB::bind_method(D_METHOD("_on_send_text", "text"), &OpencodeDockPlugin::_on_send_text);
}

void OpencodeDockPlugin::_notification(int p_notification) {
	switch (p_notification) {
		case NOTIFICATION_ENTER_TREE: {
			_enter_plugin();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_exit_plugin();
		} break;
	}
}

void OpencodeDockPlugin::_enter_plugin() {
	dock = memnew(VBoxContainer);
	dock->set_name("OpenCode");

	HBoxContainer *head = memnew(HBoxContainer);
	dock->add_child(head);
	Label *title = memnew(Label);
	title->set_text("● opencode");
	title->add_theme_color_override("font_color", Color(0.5, 0.85, 1.0));
	title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	head->add_child(title);
	model_lbl = memnew(Label);
	model_lbl->set_text(_current_model());
	model_lbl->add_theme_color_override("font_color", Color(0.6, 0.6, 0.65));
	model_lbl->add_theme_font_size_override("font_size", 12);
	head->add_child(model_lbl);

	output = memnew(RichTextLabel);
	output->set_use_bbcode(true);
	output->set_scroll_follow(true);
	output->set_selection_enabled(true);
	output->set_custom_minimum_size(Vector2(260, 160));
	output->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	dock->add_child(output);

	HBoxContainer *row = memnew(HBoxContainer);
	dock->add_child(row);
	input = memnew(LineEdit);
	input->set_placeholder("Tanya opencode... (Enter kirim)");
	input->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	row->add_child(input);
	send_btn = memnew(Button);
	send_btn->set_text("Kirim");
	row->add_child(send_btn);

	status = memnew(Label);
	status->set_text("agent: siap");
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	dock->add_child(status);

	input->connect("text_submitted", callable_mp(this, &OpencodeDockPlugin::_on_send_text));
	send_btn->connect("pressed", callable_mp(this, &OpencodeDockPlugin::_on_send));

	add_control_to_dock(DOCK_SLOT_RIGHT_BR, dock);
	_append_log("agent", "Halo! Tulis pertanyaan lalu Kirim/Enter. Tools MCP Godot tersedia.", Color(0.5, 0.85, 1.0));
}

void OpencodeDockPlugin::_exit_plugin() {
	if (dock) {
		remove_control_from_docks(dock);
		dock = nullptr;
		output = nullptr;
		input = nullptr;
		send_btn = nullptr;
		status = nullptr;
		model_lbl = nullptr;
	}
}

String OpencodeDockPlugin::_current_model() const {
	EditorSettings *es = EditorSettings::get_singleton();
	String m = es ? String(es->get_setting("agent/model")) : String();
	if (m.is_empty()) {
		m = "deepseek-v4-flash";
	}
	return m;
}

void OpencodeDockPlugin::_append_badge(const String &p_text) {
	if (!output) {
		return;
	}
	output->push_color(Color(1.0, 0.75, 0.3));
	output->add_text("  ⛭ " + p_text + "\n");
	output->pop();
}

bool OpencodeDockPlugin::_handle_slash(const String &p_text) {
	if (!p_text.begins_with("/")) {
		return false;
	}
	if (p_text == "/help" || p_text.begins_with("/help ")) {
		_append_log("agent", "Perintah: /help (bantuan ini), /model (lihat model), /new (sesi baru), /clear (bersihkan layar). Kirim teks biasa untuk bertanya.", Color(0.5, 0.85, 1.0));
	} else if (p_text == "/model" || p_text.begins_with("/model ")) {
		_append_log("agent", "Model aktif: " + _current_model() + ". Ganti di Editor Settings > agent/model.", Color(0.5, 0.85, 1.0));
	} else if (p_text == "/new") {
		agent_session = agent_chat_new_session();
		output->clear();
		_append_log("agent", "Sesi baru dimulai.", Color(0.5, 0.85, 1.0));
	} else if (p_text == "/clear") {
		output->clear();
	} else {
		_append_log("error", "Perintah tidak dikenal: " + p_text + " (coba /help)", Color(1.0, 0.45, 0.45));
	}
	return true;
}

void OpencodeDockPlugin::_append_log(const String &p_who, const String &p_text, const Color &p_color) {
	if (!output) {
		return;
	}
	output->push_color(p_color);
	output->add_text("[" + p_who + "] ");
	output->pop();
	output->add_text(p_text + "\n\n");
}

void OpencodeDockPlugin::_set_busy(bool p_busy) {
	busy = p_busy;
	if (send_btn) {
		send_btn->set_disabled(p_busy);
	}
	if (input) {
		input->set_editable(!p_busy);
	}
	if (status) {
		status->set_text(p_busy ? "agent: berpikir..." : "agent: siap");
	}
}

void OpencodeDockPlugin::_on_send() {
	if (busy || !input) {
		return;
	}
	String prompt = input->get_text().strip_edges();
	if (prompt.is_empty()) {
		return;
	}
	input->set_text("");
	_on_send_text(prompt);
}

void OpencodeDockPlugin::_on_send_text(const String &p_text) {
	if (busy || p_text.strip_edges().is_empty()) {
		return;
	}
	String prompt = p_text.strip_edges();
	output->push_color(Color(0.7, 1.0, 0.7));
	output->add_text("> " + prompt + "\n");
	output->pop();
	if (_handle_slash(prompt)) {
		return;
	}
	_set_busy(true);
	String sid = agent_session;
	std::thread([this, prompt, sid]() {
		Dictionary res = agent_chat_send(sid, prompt);
		call_deferred("_on_result", res);
	}).detach();
}

void OpencodeDockPlugin::_on_result(const Dictionary &p_res) {
	String err = p_res.get("error", String());
	String sid = String(p_res.get("session", String()));
	if (!sid.is_empty()) {
		agent_session = sid;
	}
	Array used = p_res.get("tools_used", Array());
	for (int i = 0; i < used.size(); i++) {
		_append_badge(String(used[i]));
	}
	if (!err.is_empty()) {
		_append_log("error", err, Color(1.0, 0.45, 0.45));
	} else {
		String out = String(p_res.get("text", String())).strip_edges();
		if (out.is_empty()) {
			out = "(kosong)";
		}
		_append_log("agent", out, Color(0.9, 0.9, 0.9));
	}
	_set_busy(false);
}

#endif // TOOLS_ENABLED
