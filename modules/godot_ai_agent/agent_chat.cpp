#include "agent_chat.h"

#include "core/crypto/crypto.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/object/property_info.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/config/project_settings.h"
#include "editor/settings/editor_settings.h"
#ifdef MODULE_GODOT_MCP_ENABLED
#include "modules/godot_mcp/mcp_server.h"
#endif

#include <atomic>

#define AGENT_DEFAULT_BASE_URL "https://opencode.ai/zen/v1/chat/completions"
#define AGENT_DEFAULT_MODEL "deepseek-v4-flash"
#define AGENT_DEFAULT_MAX_ROUNDS 8
#define AGENT_DEFAULT_TIMEOUT_SEC 120

// Cancellation: set from the dock stop button, polled in HTTP waits + tool loop.
static std::atomic_bool agent_cancel_requested(false);

void agent_chat_cancel() {
	agent_cancel_requested.store(true);
}

static String agent_setting_str(const String &p_name, const String &p_default) {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es && es->has_setting(p_name)) {
		return String(es->get_setting(p_name));
	}
	return p_default;
}

static int agent_setting_int(const String &p_name, int p_default) {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es && es->has_setting(p_name)) {
		return int(es->get_setting(p_name));
	}
	return p_default;
}

void agent_chat_register_settings() {
	EditorSettings *es = EditorSettings::get_singleton();
	if (!es) {
		return;
	}
	if (!es->has_setting("agent/base_url")) {
		es->set_setting("agent/base_url", String(AGENT_DEFAULT_BASE_URL));
	}
	if (!es->has_setting("agent/api_key")) {
		es->set_setting("agent/api_key", String());
	}
	if (!es->has_setting("agent/model")) {
		es->set_setting("agent/model", String(AGENT_DEFAULT_MODEL));
	}
	if (!es->has_setting("agent/max_rounds")) {
		es->set_setting("agent/max_rounds", AGENT_DEFAULT_MAX_ROUNDS);
	}
	es->add_property_hint(PropertyInfo(Variant::STRING, "agent/base_url", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Base URL API OpenAI-compatible (Zen default + custom: Nvidia, OrcaRouter, dsb)."));
	es->add_property_hint(PropertyInfo(Variant::STRING, "agent/api_key", PROPERTY_HINT_PASSWORD, "", PROPERTY_USAGE_DEFAULT, "API key provider (Zen / custom). Wajib diisi."));
	es->add_property_hint(PropertyInfo(Variant::STRING, "agent/model", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "Model ID (mis. deepseek-v4-flash, kimi-k2.6, glm-5.3-flash)."));
	es->add_property_hint(PropertyInfo(Variant::INT, "agent/max_rounds", PROPERTY_HINT_RANGE, "1,16,1"));
}

static String agent_sessions_dir() {
	return "user://agent_sessions";
}

static String agent_session_path(const String &p_id) {
	String safe;
	for (int i = 0; i < p_id.length(); i++) {
		char32_t c = p_id[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
			safe += c;
		}
	}
	if (safe.is_empty()) {
		safe = "default";
	}
	return agent_sessions_dir().path_join(safe + ".json");
}

String agent_chat_new_session() {
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(agent_sessions_dir()));
	uint64_t t = Time::get_singleton()->get_ticks_msec();
	String id = vformat("ses-%d-%d", t, Math::rand() % 100000);
	Dictionary data;
	data["messages"] = Array();
	Ref<FileAccess> f = FileAccess::open(agent_session_path(id), FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(JSON::stringify(data));
	}
	return id;
}

static Array agent_load_history(const String &p_id) {
	Array msgs;
	Ref<FileAccess> f = FileAccess::open(agent_session_path(p_id), FileAccess::READ);
	if (f.is_valid()) {
		Variant v = JSON::parse_string(f->get_as_text());
		if (v.get_type() == Variant::DICTIONARY) {
			msgs = Dictionary(v).get("messages", Array());
		}
	}
	return msgs;
}

static void agent_save_history(const String &p_id, const Array &p_msgs) {
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(agent_sessions_dir()));
	Dictionary data;
	data["messages"] = p_msgs;
	Ref<FileAccess> f = FileAccess::open(agent_session_path(p_id), FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(JSON::stringify(data));
	}
}

static String agent_tool_result_text(const Variant &p_res) {
	if (p_res.get_type() == Variant::DICTIONARY) {
		Dictionary d = p_res;
		if (d.has("content")) {
			Variant c = d["content"];
			if (c.get_type() == Variant::ARRAY) {
				String out;
				for (int i = 0; i < Array(c).size(); i++) {
					Variant item = Array(c)[i];
					if (item.get_type() == Variant::DICTIONARY && Dictionary(item).has("text")) {
						out += String(Dictionary(item)["text"]) + "\n";
					}
				}
				if (!out.is_empty()) {
					return out.strip_edges();
				}
			}
		}
	}
	if (p_res.get_type() == Variant::STRING) {
		return String(p_res);
	}
	return JSON::stringify(p_res);
}

// Blocking HTTPS POST JSON. Returns parsed response or sets r_error.
static Variant agent_http_post(const String &p_url, const String &p_key, const Dictionary &p_body, int p_timeout_sec, String &r_error) {
	String url = p_url.strip_edges();
	bool use_tls = url.begins_with("https://");
	int sep = url.find("://");
	String rest = sep == -1 ? url : url.substr(sep + 3);
	int slash = rest.find("/");
	String host = slash == -1 ? rest : rest.substr(0, slash);
	String path = slash == -1 ? "/" : rest.substr(slash);
	int port = use_tls ? 443 : 80;
	int colon = host.find(":");
	if (colon != -1) {
		port = host.substr(colon + 1).to_int();
		host = host.substr(0, colon);
	}
	Ref<TLSOptions> tls;
	if (use_tls) {
		tls = TLSOptions::client();
	}
	HTTPClient *client = HTTPClient::create();
	Error err = client->connect_to_host(host, port, tls);
	if (err != OK) {
		memdelete(client);
		r_error = "Tidak bisa konek ke " + host;
		return Variant();
	}
	uint64_t t0 = Time::get_singleton()->get_ticks_msec();
	while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout konek ke " + host;
			return Variant();
		}
		OS::get_singleton()->delay_usec(50000);
	}
	if (client->get_status() != HTTPClient::STATUS_CONNECTED) {
		memdelete(client);
		r_error = "Gagal konek ke " + host;
		return Variant();
	}
	Vector<String> headers;
	headers.append("Content-Type: application/json");
	if (!p_key.is_empty()) {
		headers.append("Authorization: Bearer " + p_key);
	}
	String body = JSON::stringify(p_body);
	CharString body_utf8 = body.utf8();
	err = client->request(HTTPClient::METHOD_POST, path, headers, (const uint8_t *)body_utf8.get_data(), body_utf8.length());
	if (err != OK) {
		memdelete(client);
		r_error = "Gagal kirim request.";
		return Variant();
	}
	t0 = Time::get_singleton()->get_ticks_msec();
	while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout tunggu respons model.";
			return Variant();
		}
		OS::get_singleton()->delay_usec(100000);
	}
	if (!client->has_response()) {
		memdelete(client);
		r_error = "Tidak ada respons dari model.";
		return Variant();
	}
	PackedByteArray bytes;
	while (client->get_status() == HTTPClient::STATUS_BODY) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		PackedByteArray chunk = client->read_response_body_chunk();
		if (chunk.size() > 0) {
			bytes.append_array(chunk);
		}
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout baca respons model.";
			return Variant();
		}
		OS::get_singleton()->delay_usec(20000);
	}
	int code = client->get_response_code();
	memdelete(client);
	String text = String::utf8((const char *)bytes.ptr(), bytes.size());
	if (code < 200 || code >= 300) {
		r_error = vformat("Provider error %d: %s", code, text.substr(0, 300));
		return Variant();
	}
	return JSON::parse_string(text);
}

static String agent_models_url(const String &p_base_url) {
	String u = p_base_url.strip_edges();
	while (u.ends_with("/")) {
		u = u.substr(0, u.length() - 1);
	}
	const String suffix = "/chat/completions";
	if (u.ends_with(suffix)) {
		u = u.substr(0, u.length() - suffix.length());
	}
	if (!u.ends_with("/models")) {
		u += "/models";
	}
	return u;
}

// Blocking HTTPS GET JSON (untuk /models). Returns parsed response or sets r_error.
static Variant agent_http_get(const String &p_url, const String &p_key, int p_timeout_sec, String &r_error) {
	String url = p_url.strip_edges();
	bool use_tls = url.begins_with("https://");
	int sep = url.find("://");
	String rest = sep == -1 ? url : url.substr(sep + 3);
	int slash = rest.find("/");
	String host = slash == -1 ? rest : rest.substr(0, slash);
	String path = slash == -1 ? "/" : rest.substr(slash);
	int port = use_tls ? 443 : 80;
	int colon = host.find(":");
	if (colon != -1) {
		port = host.substr(colon + 1).to_int();
		host = host.substr(0, colon);
	}
	Ref<TLSOptions> tls;
	if (use_tls) {
		tls = TLSOptions::client();
	}
	HTTPClient *client = HTTPClient::create();
	Error err = client->connect_to_host(host, port, tls);
	if (err != OK) {
		memdelete(client);
		r_error = "Tidak bisa konek ke " + host;
		return Variant();
	}
	uint64_t t0 = Time::get_singleton()->get_ticks_msec();
	while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout konek ke " + host;
			return Variant();
		}
		OS::get_singleton()->delay_usec(50000);
	}
	if (client->get_status() != HTTPClient::STATUS_CONNECTED) {
		memdelete(client);
		r_error = "Gagal konek ke " + host;
		return Variant();
	}
	Vector<String> headers;
	if (!p_key.is_empty()) {
		headers.append("Authorization: Bearer " + p_key);
	}
	uint8_t dummy = 0;
	err = client->request(HTTPClient::METHOD_GET, path, headers, &dummy, 0);
	if (err != OK) {
		memdelete(client);
		r_error = "Gagal kirim request.";
		return Variant();
	}
	t0 = Time::get_singleton()->get_ticks_msec();
	while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout tunggu respons.";
			return Variant();
		}
		OS::get_singleton()->delay_usec(100000);
	}
	if (!client->has_response()) {
		memdelete(client);
		r_error = "Tidak ada respons.";
		return Variant();
	}
	PackedByteArray bytes;
	while (client->get_status() == HTTPClient::STATUS_BODY) {
		if (agent_cancel_requested.load()) {
			memdelete(client);
			r_error = "Dibatalkan oleh pengguna.";
			return Variant();
		}
		client->poll();
		PackedByteArray chunk = client->read_response_body_chunk();
		if (chunk.size() > 0) {
			bytes.append_array(chunk);
		}
		if (Time::get_singleton()->get_ticks_msec() - t0 > uint64_t(p_timeout_sec) * 1000) {
			memdelete(client);
			r_error = "Timeout baca respons.";
			return Variant();
		}
		OS::get_singleton()->delay_usec(20000);
	}
	int code = client->get_response_code();
	memdelete(client);
	String text = String::utf8((const char *)bytes.ptr(), bytes.size());
	if (code < 200 || code >= 300) {
		r_error = vformat("Provider error %d: %s", code, text.substr(0, 300));
		return Variant();
	}
	return JSON::parse_string(text);
}

Dictionary agent_chat_fetch_models() {
	Dictionary res;
	res["models"] = Array();
	res["error"] = String();
	String base_url = agent_setting_str("agent/base_url", AGENT_DEFAULT_BASE_URL);
	String api_key = agent_setting_str("agent/api_key", String());
	if (api_key.strip_edges().is_empty()) {
		res["error"] = String("Isi API key dulu di Editor Settings > agent/api_key.");
		return res;
	}
	String err;
	Variant resp = agent_http_get(agent_models_url(base_url), api_key, 30, err);
	if (!err.is_empty()) {
		res["error"] = err;
		return res;
	}
	Array ids;
	if (resp.get_type() == Variant::DICTIONARY) {
		Array data = Dictionary(resp).get("data", Array());
		for (int i = 0; i < data.size(); i++) {
			if (Dictionary(data[i]).has("id")) {
				ids.append(String(Dictionary(data[i])["id"]));
			}
		}
	}
	ids.sort();
	res["models"] = ids;
	return res;
}

static Dictionary agent_system_message() {
	Dictionary m;
	m["role"] = "system";
	m["content"] = "Kamu asisten AI di dalam editor Godot (Godot Agent). Jawab singkat Bahasa Indonesia. "
				   "Kamu punya TOOLS untuk membaca/mengubah scene, node, script, project, dan menjalankan game. "
				   "Gunakan tools saat user minta aksi konkret di editor; jika hanya bertanya, jawab langsung. "
				   "Jangan menebak isi file: baca dulu bila perlu.";
	return m;
}

Dictionary agent_chat_send(const String &p_session_id, const String &p_prompt) {
	Dictionary res;
	res["text"] = String();
	res["tools_used"] = Array();
	res["session"] = p_session_id.is_empty() ? agent_chat_new_session() : p_session_id;
	res["error"] = String();

	String base_url = agent_setting_str("agent/base_url", AGENT_DEFAULT_BASE_URL);
	String api_key = agent_setting_str("agent/api_key", String());
	String model = agent_setting_str("agent/model", AGENT_DEFAULT_MODEL);
	int max_rounds = agent_setting_int("agent/max_rounds", AGENT_DEFAULT_MAX_ROUNDS);
	int timeout = agent_setting_int("agent/timeout_sec", AGENT_DEFAULT_TIMEOUT_SEC);
	if (api_key.strip_edges().is_empty()) {
		res["error"] = String("Isi API key dulu di Editor Settings > agent/api_key (Zen / Nvidia / OrcaRouter).");
		return res;
	}

	String sid = String(res["session"]);
	agent_cancel_requested.store(false);
	Array history = agent_load_history(sid);
	Dictionary user_msg;
	user_msg["role"] = "user";
	user_msg["content"] = p_prompt;
	history.append(user_msg);

#ifdef MODULE_GODOT_MCP_ENABLED
	McpServer *srv = McpServer::get_singleton();
	Array tool_defs = srv ? srv->list_tool_defs() : Array();
#else
	Array tool_defs;
#endif
	Array tools;
	for (int i = 0; i < tool_defs.size(); i++) {
		Dictionary td = tool_defs[i];
		Dictionary fn;
		fn["name"] = td.get("name", String());
		fn["description"] = td.get("description", String());
		fn["parameters"] = td.get("schema", Dictionary());
		Dictionary t;
		t["type"] = "function";
		t["function"] = fn;
		tools.append(t);
	}

	Array tools_used;
	String final_text;
	String cancelled;
	for (int round = 0; round < max_rounds; round++) {
		if (agent_cancel_requested.load()) {
			cancelled = "Dibatalkan oleh pengguna.";
			break;
		}
		Dictionary body;
		body["model"] = model;
		Array msgs;
		msgs.append(agent_system_message());
		msgs.append_array(history);
		body["messages"] = msgs;
		if (!tools.is_empty()) {
			body["tools"] = tools;
		}
		String err;
		Variant resp = agent_http_post(base_url, api_key, body, timeout, err);
		if (!err.is_empty()) {
			res["error"] = err;
			break;
		}
		if (resp.get_type() != Variant::DICTIONARY) {
			res["error"] = String("Respons provider tidak valid.");
			break;
		}
		Dictionary rd = resp;
		Array choices = rd.get("choices", Array());
		if (choices.is_empty()) {
			res["error"] = String("Provider tidak mengembalikan choices: " + JSON::stringify(rd).substr(0, 300));
			break;
		}
		Dictionary msg = Dictionary(choices[0]).get("message", Dictionary());
		String content = String(msg.get("content", String()));
		Array calls = msg.get("tool_calls", Array());
		Dictionary asst;
		asst["role"] = "assistant";
		if (!content.is_empty()) {
			asst["content"] = content;
		}
		if (!calls.is_empty()) {
			asst["tool_calls"] = calls;
		}
		history.append(asst);
		if (calls.is_empty()) {
			final_text = content;
			break;
		}
		for (int i = 0; i < calls.size(); i++) {
			if (agent_cancel_requested.load()) {
				cancelled = "Dibatalkan oleh pengguna.";
				break;
			}
			Dictionary call = calls[i];
			String call_id = String(call.get("id", String()));
			Dictionary fn = call.get("function", Dictionary());
			String fname = String(fn.get("name", String()));
			String fargs_s = String(fn.get("arguments", String("{}")));
			Dictionary fargs;
			Variant av = JSON::parse_string(fargs_s);
			if (av.get_type() == Variant::DICTIONARY) {
				fargs = av;
			}
			tools_used.append(fname);
			String tout;
#ifdef MODULE_GODOT_MCP_ENABLED
			McpServer *srv2 = McpServer::get_singleton();
			if (srv2) {
				tout = agent_tool_result_text(srv2->execute_tool(fname, fargs));
			} else {
				tout = "MCP server tidak tersedia.";
			}
#else
			tout = "MCP server tidak tersedia (modul godot_mcp mati).";
#endif
			Dictionary tmsg;
			tmsg["role"] = "tool";
			tmsg["tool_call_id"] = call_id;
			tmsg["content"] = tout;
			history.append(tmsg);
		}
		if (!cancelled.is_empty()) {
			break;
		}
		agent_save_history(sid, history);
	}
	if (!cancelled.is_empty()) {
		res["error"] = cancelled;
	} else if (final_text.is_empty() && String(res["error"]).is_empty()) {
		final_text = "(model tidak mengembalikan teks)";
	}
	agent_save_history(sid, history);
	res["text"] = final_text;
	res["tools_used"] = tools_used;
	return res;
}
