#include "mcp_server.h"

#include "core/config/engine.h"
#include "core/object/message_queue.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/thread.h"
#include "core/os/time.h"
#include "core/string/string_name.h"
#include "core/io/ip.h"
#include "core/io/json.h"
#include "core/object/property_info.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "mcp_android.h"
#include "mcp_http.h"
#include "mcp_tools.h"
#include "notch_hider.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include <chrono>

McpServer *McpServer::singleton = nullptr;

McpServer::McpServer() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "McpServer already exists.");
	singleton = this;
}

McpServer::~McpServer() {
	stop_server();
	singleton = nullptr;
}

McpServer *McpServer::get_singleton() {
	if (!singleton) {
		singleton = memnew(McpServer);
	}
	return singleton;
}

void McpServer::cleanup() {
	if (singleton) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void McpServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_server"), &McpServer::start_server);
	ClassDB::bind_method(D_METHOD("stop_server"), &McpServer::stop_server);
	ClassDB::bind_method(D_METHOD("is_running"), &McpServer::is_running);
	ClassDB::bind_method(D_METHOD("start_if_enabled"), &McpServer::start_if_enabled);
	ClassDB::bind_method(D_METHOD("apply_config"), &McpServer::apply_config);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &McpServer::set_enabled);
	ClassDB::bind_method(D_METHOD("get_mcp_url"), &McpServer::get_mcp_url);
}

bool McpServer::get_enabled() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return false;
	}
		if (!es->has_setting("network/mcp/enabled")) {
		return false;
	}
	return bool(es->get_setting("network/mcp/enabled"));
}

int McpServer::get_port() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return 8766;
	}
		if (!es->has_setting("network/mcp/port")) {
		return 8766;
	}
	return int(es->get_setting("network/mcp/port"));
}

int McpServer::read_log_level_setting() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return 2;
	}
	if (!es->has_setting("network/mcp/log_level")) {
		return 2;
	}
	return int(es->get_setting("network/mcp/log_level"));
}

// Prefix "[HH:MM:SS] " untuk baris log MCP (Fase 1D).
static String _mcp_stamp() {
	Time *t = Time::get_singleton();
	if (!t) {
		return String();
	}
	return "[" + t->get_time_string_from_system() + "] ";
}

String McpServer::get_bind() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return "0.0.0.0";
	}
	if (!es->has_setting("network/mcp/bind_mode")) {
		return "0.0.0.0";
	}
	return int(es->get_setting("network/mcp/bind_mode")) == 1 ? "127.0.0.1" : "0.0.0.0";
}

int McpServer::get_bind_mode() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return 0;
	}
	if (!es->has_setting("network/mcp/bind_mode")) {
		return 0;
	}
	return int(es->get_setting("network/mcp/bind_mode"));
}

int McpServer::get_transport() const {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return 0;
	}
	if (!es->has_setting("network/mcp/transport")) {
		return 0;
	}
	return int(es->get_setting("network/mcp/transport"));
}

String McpServer::get_local_ip() const {
	List<IPAddress> addrs;
	IP::get_singleton()->get_local_addresses(&addrs);
	String fallback;
	for (const IPAddress &a : addrs) {
		String s = String(a);
		if (s.begins_with("127.") || s.begins_with("::") || s == "0.0.0.0" || s.contains(":")) {
			continue;
		}
		if (s.begins_with("192.168.") || s.begins_with("10.") || s.begins_with("172.")) {
			return s;
		}
		if (fallback.is_empty()) {
			fallback = s;
		}
	}
	if (!fallback.is_empty()) {
		return fallback;
	}
	return "127.0.0.1";
}

void McpServer::set_enabled(bool p_enabled) {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return;
	}
	es->set_setting("network/mcp/enabled", p_enabled);
	es->save();
	if (p_enabled) {
		start_if_enabled();
	} else {
		stop_server();
	}
}

String McpServer::get_mcp_url() const {
	String host = get_bind();
	if (host == "0.0.0.0") {
		host = get_local_ip();
	}
	String path = get_transport() == 2 ? "/sse" : "/mcp";
	return "http://" + host + ":" + itos(get_port()) + path;
}

void McpServer::apply_config() {
	bool was_running = running;
	if (was_running) {
		stop_server();
	}
	if (get_enabled()) {
		start_server();
	}
}

void McpServer::start_if_enabled() {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (running) {
		return;
	}
	if (!get_enabled()) {
		return;
	}
	start_server();
}

void McpServer::start_server() {
	if (running) {
		return;
	}
	_update_from_settings();
	cfg_port = get_port();
	cfg_bind_mode = get_bind_mode();
	cfg_transport = get_transport();
	cfg_log_level = read_log_level_setting();
#ifdef TOOLS_ENABLED
	_register_builtin_tools();
#endif
	http = std::make_unique<MCPHttpServer>(this);
	if (!http->start()) {
		http.reset();
		ERR_PRINT(vformat("Godot MCP: gagal mengikat %s:%d", get_bind(), get_port()));
		return;
	}
	running = true;
	was_playing = false;
	last_scene_id = 0;
	last_tick = 0;
	SceneTree *st = SceneTree::get_singleton();
	if (st && !tick_connected) {
		st->connect(SNAME("process_frame"), callable_mp(this, &McpServer::_tick));
		tick_connected = true;
	}
	if (cfg_log_level >= 1) {
		print_line(vformat("%sGodot MCP: server berjalan di %s", _mcp_stamp(), get_mcp_url()));
		mcp_log_append(vformat("%sGodot MCP: server berjalan di %s", _mcp_stamp(), get_mcp_url()));
	}
#ifdef ANDROID_ENABLED
	mcp_android_notification_on();
#endif
}

void McpServer::stop_server() {
	if (!running) {
		return;
	}
	running = false;
	if (http) {
		http->stop();
		http.reset();
	}
	{
		std::lock_guard<std::mutex> lk(sessions_mu);
		sessions.clear();
	}
#ifdef ANDROID_ENABLED
	mcp_android_notification_off();
#endif
	print_verbose("Godot MCP: server dihentikan");
	if (cfg_log_level >= 1) {
		mcp_log_append(vformat("%sGodot MCP: server dihentikan", _mcp_stamp()));
	}
}

void McpServer::register_editor_settings() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return;
	}
	// Sekali migrasi: pindahkan nilai lama mcp/* ke network/mcp/* (Editor Settings > Jaringan).
	// Aman diulang: hanya menyalin bila kunci lama ada dan kunci baru belum ada.
	const char *migrated_keys[] = { "enabled", "port", "transport", "bind_mode", "auto_reload_external", "log_level" };
	for (const char *k : migrated_keys) {
		String oldk = String("mcp/") + k;
		String newk = String("network/mcp/") + k;
		if (es->has_setting(oldk) && !es->has_setting(newk)) {
			es->set_setting(newk, es->get_setting(oldk));
		}
		if (es->has_setting(oldk)) {
			es->erase(oldk);
		}
	}
	if (!es->has_setting("network/mcp/enabled")) {
		es->set_setting("network/mcp/enabled", false);
	}
	if (!es->has_setting("network/mcp/port")) {
		es->set_setting("network/mcp/port", 8766);
	}
	if (!es->has_setting("network/mcp/transport")) {
		es->set_setting("network/mcp/transport", 0);
	}
	if (!es->has_setting("network/mcp/bind_mode")) {
		es->set_setting("network/mcp/bind_mode", 0);
	}
	if (!es->has_setting("network/mcp/auto_reload_external")) {
		es->set_setting("network/mcp/auto_reload_external", false);
	}
	if (!es->has_setting("network/mcp/log_level")) {
		es->set_setting("network/mcp/log_level", 2);
	}
	// Hide-notch toggle (Editor Settings: android/hide_display_cutout, default ON).
	notch_hider_register_settings();
	notch_hider_apply();
	// The Android editor fork lacks GDK window placement settings; define them
	// so running the project stops spamming ERR_PRINT "does not exist" errors.
	if (!es->has_setting("run/window_placement/screen")) {
		es->set_setting("run/window_placement/screen", 0);
	}
	if (!es->has_setting("run/window_placement/rect")) {
		es->set_setting("run/window_placement/rect", Rect2i(0, 0, 0, 0));
	}
	es->add_property_hint(PropertyInfo(Variant::BOOL, "network/mcp/enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Jalankan server MCP saat editor terbuka"));
	es->add_property_hint(PropertyInfo(Variant::INT, "network/mcp/transport", PROPERTY_HINT_ENUM, "Keduanya (Streamable HTTP + SSE),Hanya Streamable HTTP,Hanya SSE"));
	es->add_property_hint(PropertyInfo(Variant::INT, "network/mcp/bind_mode", PROPERTY_HINT_ENUM, "LAN (bisa diakses dari perangkat lain),Hanya Localhost"));
	es->add_property_hint(PropertyInfo(Variant::INT, "network/mcp/port", PROPERTY_HINT_RANGE, "1,65535,1"));
	es->add_property_hint(PropertyInfo(Variant::BOOL, "network/mcp/auto_reload_external", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Muat ulang otomatis file yang berubah di luar editor (tanpa konfirmasi)"));
	es->add_property_hint(PropertyInfo(Variant::INT, "network/mcp/log_level", PROPERTY_HINT_ENUM, "Diam,Hanya koneksi,Semua"));
	// The MCP server never auto-starts on launch. Keep the poll connected so
	// enabling/disabling the setting takes effect immediately without a restart.
	McpServer *s = McpServer::get_singleton();
	SceneTree *st = SceneTree::get_singleton();
	if (s && st && !s->tick_connected) {
		st->connect(SNAME("process_frame"), callable_mp(s, &McpServer::_tick));
		s->tick_connected = true;
	}
}

void McpServer::_update_from_settings() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es) {
		if (!es->has_setting("network/mcp/enabled")) {
			es->set_setting("network/mcp/enabled", false);
		}
		if (!es->has_setting("network/mcp/port")) {
			es->set_setting("network/mcp/port", 8766);
		}
		if (!es->has_setting("network/mcp/transport")) {
			es->set_setting("network/mcp/transport", 0);
		}
		if (!es->has_setting("network/mcp/bind_mode")) {
			es->set_setting("network/mcp/bind_mode", 0);
		}
		if (!es->has_setting("network/mcp/log_level")) {
			es->set_setting("network/mcp/log_level", 2);
		}
	}
}

String McpServer::_new_session_id() {
	String base = String::num_uint64(Time::get_singleton()->get_ticks_usec(), 16) + String::num_uint64((uint64_t)(uintptr_t)this, 16);
	String id = base.md5_text() + base.sha1_text().substr(0, 8);
	return id.to_lower();
}

Variant McpServer::_handle_request(const String &p_session_id, const Variant &p_message, bool &r_broadcast_session, String &r_created_session, const String &p_peer_ip) {
	r_broadcast_session = false;
	if (p_message.get_type() != Variant::DICTIONARY) {
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", Variant() }, { "error", Dictionary{ { "code", -32600 }, { "message", "Invalid Request" } } } };
	}
	Dictionary msg = p_message;
	String method = msg.get("method", String());
	Variant id = msg.get("id", Variant());
	Dictionary params = msg.get("params", Dictionary());

	if (method == "initialize") {
		String client_name = "unknown";
		String client_version = "";
		if (params.has("clientInfo")) {
			Dictionary ci = params["clientInfo"];
			client_name = ci.get("name", "unknown");
			client_version = ci.get("version", "");
		}
		String proto = params.get("protocolVersion", "2024-11-05");
		String sid = p_session_id;
		if (sid.is_empty()) {
			sid = _new_session_id();
			r_created_session = sid;
		}
		{
			std::lock_guard<std::mutex> lk(sessions_mu);
			Session s;
			s.id = sid;
			s.protocol_version = proto;
			s.peer_ip = p_peer_ip;
			sessions[sid] = s;
		}
		protocol_version = proto;
		if (cfg_log_level >= 1) {
			String from = p_peer_ip.is_empty() ? String() : vformat(" dari %s", p_peer_ip);
			print_line(vformat("%sGodot MCP: klien '%s %s' terhubung (session %s, protocol %s%s)", _mcp_stamp(), client_name, client_version, sid, proto, from));
			mcp_log_append(vformat("%sGodot MCP: klien '%s %s' terhubung (session %s, protocol %s%s)", _mcp_stamp(), client_name, client_version, sid, proto, from));
		}
		Dictionary caps;
		caps["tools"] = Dictionary{ { "listChanged", true } };
		caps["logging"] = Dictionary{};
		Dictionary result;
		result["protocolVersion"] = proto;
		result["capabilities"] = caps;
		result["serverInfo"] = Dictionary{ { "name", server_name }, { "version", server_version } };
		Dictionary d;
		d["jsonrpc"] = "2.0";
		d["id"] = id;
		d["result"] = result;
		return d;
	}

	if (method == "notifications/initialized" || method == "initialized") {
		return Variant();
	}

	if (method == "ping") {
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Dictionary{} } };
	}

	if (method == "tools/list" || method == "tools/list_changed") {
		Array tools_arr;
#ifdef TOOLS_ENABLED
		{
			std::lock_guard<std::mutex> lk(tools_mu);
			for (const auto &pair : tools) {
				Dictionary t;
				t["name"] = pair.second.name;
				t["description"] = pair.second.description;
				t["inputSchema"] = pair.second.schema;
				tools_arr.append(t);
			}
		}
#endif
		Dictionary result_root;
		result_root["tools"] = tools_arr;
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result_root } };
	}

	if (method == "tools/call") {
		String name = params.get("name", String());
		Variant av = params.get("arguments", Variant());
		Dictionary arguments;
		if (av.get_type() == Variant::DICTIONARY) {
			arguments = av;
		} else if (av.get_type() == Variant::ARRAY) {
			// Some clients send a raw array as 'arguments' (batch_execute list).
			arguments["operations"] = av;
		}
		Dictionary result_root = _execute_tool(name, arguments);
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result_root } };
	}

	if (method == "resources/list") {
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Dictionary{ { "resources", Array() } } } };
	}

	if (method == "logging/setLevel") {
		return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "result", Dictionary{} } };
	}

	if (method == "shutdown" || method == "exit") {
		return Variant();
	}

	return Dictionary{ { "jsonrpc", "2.0" }, { "id", id }, { "error", Dictionary{ { "code", -32601 }, { "message", vformat("Method not found: %s", method) } } } };
}

Dictionary McpServer::handle_jsonrpc(const String &p_session_id, const Variant &p_message, bool &r_broadcast_session, String &r_created_session, const String &p_peer_ip) {
	Variant v = _handle_request(p_session_id, p_message, r_broadcast_session, r_created_session, p_peer_ip);
	if (v.get_type() == Variant::NIL) {
		return Dictionary();
	}
	return v;
}

Variant McpServer::execute_tool(const String &p_name, const Dictionary &p_args) {
	return _execute_tool(p_name, p_args);
}

Dictionary McpServer::_execute_tool(const String &p_name, const Dictionary &p_arguments) {
#ifdef TOOLS_ENABLED
	ToolDef def;
	{
		std::lock_guard<std::mutex> lk(tools_mu);
		auto it = tools.find(p_name);
		if (it == tools.end()) {
			return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", vformat("Unknown tool: %s", p_name) } } } }, { "isError", true } };
		}
		def = it->second;
	}
	Variant out = run_tool(def.handler, p_arguments);
	if (out.get_type() == Variant::DICTIONARY) {
		Dictionary d = out;
		if (d.has("content")) {
			return d;
		}
	}
	// Fallback: wrap the raw result as text.
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = JSON::stringify(out);
	return Dictionary{ { "content", Array{ content_item } }, { "isError", false } };
#else
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", "MCP tools unavailable in this build." } } } }, { "isError", true } };
#endif
}

void McpServer::_send_sse_to_session(const Session &p_session, const String &p_frame) {
	if (!p_session.sse_conn) {
		return;
	}
	mcp_http_send_frame(p_session.sse_conn, p_frame);
}

void McpServer::broadcast_notification(const String &p_method, const Dictionary &p_params) {
	if (!running) {
		return;
	}
	Dictionary msg;
	msg["jsonrpc"] = "2.0";
	msg["method"] = p_method;
	msg["params"] = p_params;
	String frame = "event: message\ndata: " + JSON::stringify(msg) + "\n\n";
	std::lock_guard<std::mutex> lk(sessions_mu);
	for (auto &pair : sessions) {
		_send_sse_to_session(pair.second, frame);
	}
}

void McpServer::_register_builtin_tools() {
	mcp_register_tools(this);
}

void McpServer::register_tool(const String &p_name, const String &p_description, const Dictionary &p_schema, Variant (*p_handler)(const Dictionary &p_args)) {
#ifdef TOOLS_ENABLED
	std::lock_guard<std::mutex> lk(tools_mu);
	ToolDef def;
	def.name = p_name;
	def.description = p_description;
	def.schema = p_schema;
	def.handler = p_handler;
	tools[p_name] = def;
#endif
}

void McpServer::_emit_events() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return;
	}
#ifdef TOOLS_ENABLED
	bool playing = ei->is_playing_scene();
	if (playing != was_playing) {
		was_playing = playing;
		broadcast_notification(playing ? "notifications/game_started" : "notifications/game_stopped", Dictionary{ { "playing", playing } });
	}
#endif
	Node *root = ei->get_edited_scene_root();
	uint64_t sid = 0;
	if (root) {
		sid = root->get_instance_id();
		String path = root->get_scene_file_path();
		if (sid != last_scene_id) {
			last_scene_id = sid;
			broadcast_notification("notifications/scene_changed", Dictionary{ { "path", path }, { "name", root->get_name() }, { "class", root->get_class() } });
		}
	} else if (last_scene_id != 0) {
		last_scene_id = 0;
		broadcast_notification("notifications/scene_changed", Dictionary{ { "path", String() }, { "name", String() }, { "class", String() } });
	}
}

void McpServer::_tick() {
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (last_tick == 0) {
		last_tick = now;
	}
	if (now - last_tick < 250) {
		return;
	}
	last_tick = now;
#ifdef TOOLS_ENABLED
	// Poll the filesystem so files changed on disk (scenes, scripts,
	// project.godot, new assets) show up in the editor right away, without
	// waiting for a game run or an application focus event. On Android the
	// window never changes focus, so the engine's focus-based scan never ran.
	// This runs as long as the editor is open, regardless of the MCP server
	// being enabled. Tool handlers also trigger an immediate reload after
	// writing files; this slower poll catches changes made outside MCP.
	if (now - last_fs_poll_msec >= 2000) {
		last_fs_poll_msec = now;
		EditorFileSystem *efs = EditorFileSystem::get_singleton();
		if (efs) {
			efs->scan_changes();
		}
		EditorNode *en = EditorNode::get_singleton();
		if (en && !EditorInterface::get_singleton()->is_playing_scene()) {
			if (EditorSettings::get_singleton()->get_setting("network/mcp/auto_reload_external")) {
				en->refresh_external_changes();
			} else {
				en->poll_external_changes();
			}
		}
		// Re-apply hide-notch toggle live (cheap: JNI only runs on change).
		notch_hider_apply();
	}
#endif
	if (!running) {
		start_if_enabled();
		return;
	}
	if (!get_enabled()) {
		stop_server();
		return;
	}
	// Level log dibaca live tanpa restart server.
	cfg_log_level = read_log_level_setting();
	{
		int p = get_port();
		int b = get_bind_mode();
		int t = get_transport();
		if (p != cfg_port || b != cfg_bind_mode || t != cfg_transport) {
			apply_config();
			return;
		}
	}
	_emit_events();
}

Variant McpServer::run_tool(Variant (*p_handler)(const Dictionary &p_args), const Dictionary &p_arguments, int p_timeout_ms) {
	if (Thread::is_main_thread()) {
		return p_handler(p_arguments);
	}
#ifdef TOOLS_ENABLED
	MainThreadTask task;
	task.handler = p_handler;
	task.args = p_arguments;
	{
		std::lock_guard<std::mutex> lk(queue_mu);
		queue.push_back(&task);
	}
	MessageQueue::get_singleton()->push_callable(callable_mp(this, &McpServer::_drain_tasks));
	{
		std::unique_lock<std::mutex> lk(task.mu);
		task.cv.wait_for(lk, std::chrono::milliseconds(p_timeout_ms), [&] { return task.done; });
	}
	if (!task.done) {
		task.done = true;
		ERR_PRINT("Godot MCP: tool kehabisan waktu di thread utama");
		return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", "Thread utama kehabisan waktu (timeout)" } } } }, { "isError", true } };
	}
	return task.result;
#else
	String err = "Eksekusi thread utama tidak tersedia.";
	Dictionary econtent;
	econtent["type"] = "text";
	econtent["text"] = err;
	return Dictionary{ { "content", Array{ econtent } }, { "isError", true } };
#endif
}

void McpServer::_drain_tasks() {
#ifdef TOOLS_ENABLED
	std::vector<MainThreadTask *> tasks;
	{
		std::lock_guard<std::mutex> lk(queue_mu);
		tasks.swap(queue);
	}
	for (MainThreadTask *t : tasks) {
		std::lock_guard<std::mutex> lk(t->mu);
		if (!t->done) {
			t->result = t->handler(t->args);
			t->done = true;
		}
		t->cv.notify_all();
	}
#endif
}