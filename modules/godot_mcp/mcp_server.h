#ifndef GODOT_MCP_SERVER_H
#define GODOT_MCP_SERVER_H

#include "core/object/object.h"
#include "core/string/ustring.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class MCPHttpServer;

class McpServer : public Object {
	GDCLASS(McpServer, Object);

	static McpServer *singleton;

	friend class MCPHttpServer;

#ifdef TOOLS_ENABLED
	struct MainThreadTask {
		std::condition_variable cv;
		std::mutex mu;
		bool done = false;
		Variant result;
		Variant (*handler)(const Dictionary &p_args) = nullptr;
		Dictionary args;
	};
#endif

public:
	struct Session {
		String id;
		String protocol_version;
		void *sse_conn = nullptr;
	};

	McpServer();
	~McpServer();

	static McpServer *get_singleton();
	static void cleanup();
	static void register_editor_settings();

	void start_if_enabled();
	void start_server();
	void stop_server();

	bool is_running() const { return running; }
	int get_port() const;
	String get_bind() const;
	bool get_enabled() const;
	int get_transport() const;
	int get_bind_mode() const;
	String get_local_ip() const;
	void apply_config();
	void set_enabled(bool p_enabled);
	String get_mcp_url() const;

	Dictionary handle_jsonrpc(const String &p_session_id, const Variant &p_message, bool &r_broadcast_session, String &r_created_session);
	void broadcast_notification(const String &p_method, const Dictionary &p_params);

	Variant run_tool(Variant (*p_handler)(const Dictionary &p_args), const Dictionary &p_arguments, int p_timeout_ms = 20000);
	Variant execute_tool(const String &p_name, const Dictionary &p_args);

	void register_tool(const String &p_name, const String &p_description, const Dictionary &p_schema, Variant (*p_handler)(const Dictionary &p_args));

protected:
	static void _bind_methods();

private:
	void _tick();
	void _drain_tasks();
	Dictionary _execute_tool(const String &p_name, const Dictionary &p_arguments);
	Variant _handle_request(const String &p_session_id, const Variant &p_message, bool &r_broadcast_session, String &r_created_session);
	String _new_session_id();
	void _register_builtin_tools();
	void _emit_events();
	void _send_sse_to_session(const Session &p_session, const String &p_frame);
	void _update_from_settings();

	std::unique_ptr<MCPHttpServer> http;
	bool running = false;
	bool tick_connected = false;
	uint64_t last_tick = 0;
	uint64_t last_fs_poll_msec = 0;
	bool was_playing = false;

	String protocol_version = "2024-11-05";
	String server_name = "godot-mcp";
	String server_version = "4.8-dev";

	std::map<String, Session> sessions;
	std::mutex sessions_mu;

#ifdef TOOLS_ENABLED
	struct ToolDef {
		String name;
		String description;
		Dictionary schema;
		Variant (*handler)(const Dictionary &p_args);
	};
	std::map<String, ToolDef> tools;
	std::mutex tools_mu;

	std::vector<MainThreadTask *> queue;
	std::mutex queue_mu;
#endif

	Object *root_node = nullptr;
	uint64_t last_scene_id = 0;

	int cfg_port = 8766;
	int cfg_bind_mode = 0;
	int cfg_transport = 0;
};

#endif // GODOT_MCP_SERVER_H