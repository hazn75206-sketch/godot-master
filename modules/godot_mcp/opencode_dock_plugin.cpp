#include "opencode_dock_plugin.h"

#ifdef TOOLS_ENABLED

#include "opencode_runner.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/variant/dictionary.h"

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
	status->set_text("opencode: siap (model gratis)");
	status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	dock->add_child(status);

	input->connect("text_submitted", callable_mp(this, &OpencodeDockPlugin::_on_send_text));
	send_btn->connect("pressed", callable_mp(this, &OpencodeDockPlugin::_on_send));

	add_control_to_dock(DOCK_SLOT_RIGHT_BR, dock);
	_append_log("opencode", "Halo! Tulis pertanyaan lalu Kirim/Enter. Tools MCP Godot tersedia.", Color(0.5, 0.85, 1.0));
}

void OpencodeDockPlugin::_exit_plugin() {
	if (dock) {
		remove_control_from_docks(dock);
		dock = nullptr;
		output = nullptr;
		input = nullptr;
		send_btn = nullptr;
		status = nullptr;
	}
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
		status->set_text(p_busy ? "opencode: berpikir..." : "opencode: siap (model gratis)");
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
	_append_log("kamu", p_text, Color(0.7, 1.0, 0.7));
	_set_busy(true);
	std::thread([this, p_text]() {
		Dictionary res = opencode_runner_run(p_text);
		call_deferred("_on_result", res);
	}).detach();
}

void OpencodeDockPlugin::_on_result(const Dictionary &p_res) {
	String err = p_res.get("error", String());
	String out = p_res.get("output", String());
	if (!err.is_empty()) {
		_append_log("error", err, Color(1.0, 0.45, 0.45));
	} else if (out.is_empty()) {
		_append_log("opencode", "(kosong)", Color(0.6, 0.6, 0.6));
	} else {
		// `opencode run --format json` emits line-delimited JSON; show text parts.
		String shown;
		for (const String &line : out.split("\n")) {
			String l = line.strip_edges();
			if (l.is_empty()) {
				continue;
			}
			Variant v = JSON::parse_string(l);
			if (v.get_type() == Variant::DICTIONARY) {
				Dictionary d = v;
				if (String(d.get("type", String())) == "text" && d.has("part")) {
					Dictionary part = d["part"];
					shown += String(part.get("text", String()));
					continue;
				}
			}
			shown += l + "\n";
		}
		_append_log("opencode", shown.strip_edges(), Color(0.9, 0.9, 0.9));
	}
	_set_busy(false);
}

#endif // TOOLS_ENABLED
