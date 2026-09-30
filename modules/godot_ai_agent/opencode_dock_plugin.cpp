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
	ClassDB::bind_method(D_METHOD("_on_models", "res"), &OpencodeDockPlugin::_on_models);
	ClassDB::bind_method(D_METHOD("_on_model_selected", "idx"), &OpencodeDockPlugin::_on_model_selected);
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
	title->set_text("● agent");
	title->add_theme_color_override("font_color", Color(0.5, 0.85, 1.0));
	title->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	head->add_child(title);
	model_lbl = memnew(Label);
	model_lbl->set_text(_current_model());
	model_lbl->add_theme_color_override("font_color", Color(0.6, 0.6, 0.65));
	model_lbl->add_theme_font_size_override("font_size", 12);
	head->add_child(model_lbl);

	HBoxContainer *modelrow = memnew(HBoxContainer);
	dock->add_child(modelrow);
	model_opt = memnew(OptionButton);
	model_opt->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	modelrow->add_child(model_opt);
	load_btn = memnew(Button);
	load_btn->set_text("Muat");
	load_btn->set_tooltip_text("Ambil daftar model dari provider (agent/base_url + agent/api_key)");
	modelrow->add_child(load_btn);
	load_btn->connect("pressed", callable_mp(this, &OpencodeDockPlugin::_on_load_models));
	model_opt->connect("item_selected", callable_mp(this, &OpencodeDockPlugin::_on_model_selected));

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
	cancel_btn = memnew(Button);
	cancel_btn->set_text("Batal");
	cancel_btn->set_disabled(true);
	row->add_child(cancel_btn);

	status = memnew(Label);
	status->set_text("agent: siap");
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	dock->add_child(status);

	input->connect("text_submitted", callable_mp(this, &OpencodeDockPlugin::_on_send_text));
	send_btn->connect("pressed", callable_mp(this, &OpencodeDockPlugin::_on_send));
	cancel_btn->connect("pressed", callable_mp(this, &OpencodeDockPlugin::_on_cancel));

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
		cancel_btn = nullptr;
		model_opt = nullptr;
		load_btn = nullptr;
		status = nullptr;
		model_lbl = nullptr;
	}
}

String OpencodeDockPlugin::_current_model() const {
	String m = _current_model_full();
	int slash = m.find("/");
	return slash == -1 ? m : m.substr(slash + 1);
}

String OpencodeDockPlugin::_current_model_full() const {
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
	if (cancel_btn) {
		cancel_btn->set_disabled(!p_busy);
	}
	if (input) {
		input->set_editable(!p_busy);
	}
	if (status) {
		status->set_text(p_busy ? "agent: berpikir..." : "agent: siap");
	}
}

void OpencodeDockPlugin::_on_cancel() {
	if (!busy) {
		return;
	}
	agent_chat_cancel();
	_append_log("agent", "Permintaan pembatalan dikirim...", Color(0.6, 0.6, 0.6));
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

void OpencodeDockPlugin::_on_load_models() {
	if (busy) {
		return;
	}
	_set_busy(true);
	if (status) {
		status->set_text("agent: memuat daftar model...");
	}
	std::thread([this]() {
		Dictionary res = agent_chat_fetch_models();
		call_deferred("_on_models", res);
	}).detach();
}

void OpencodeDockPlugin::_on_models(const Dictionary &p_res) {
	_set_busy(false);
	String err = p_res.get("error", String());
	if (!err.is_empty()) {
		_append_log("error", err, Color(1.0, 0.45, 0.45));
		return;
	}
	Array models = p_res.get("models", Array());
	if (!model_opt) {
		return;
	}
	model_opt->clear();
	String cur = _current_model_full();
	int sel = 0;
	for (int i = 0; i < models.size(); i++) {
		String m = String(models[i]);
		model_opt->add_item(m, i);
		if (m == cur) {
			sel = i;
		}
	}
	if (models.is_empty()) {
		model_opt->add_item("(tidak ada model)", 0);
	} else {
		model_opt->select(sel);
	}
	_append_log("agent", vformat("%d model dimuat dari provider. Pilih dari daftar, tanpa ketik manual.", models.size()), Color(0.5, 0.85, 1.0));
}

void OpencodeDockPlugin::_on_model_selected(int p_idx) {
	if (!model_opt) {
		return;
	}
	String m = model_opt->get_item_text(p_idx);
	if (m.is_empty() || m.begins_with("(")) {
		return;
	}
	EditorSettings *es = EditorSettings::get_singleton();
	if (es) {
		es->set_setting("agent/model", m);
	}
	if (model_lbl) {
		model_lbl->set_text(_current_model());
	}
	_append_log("agent", "Model aktif: " + m, Color(0.5, 0.85, 1.0));
}

#endif // TOOLS_ENABLED
