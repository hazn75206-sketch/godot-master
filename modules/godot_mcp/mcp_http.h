#ifndef GODOT_MCP_HTTP_H
#define GODOT_MCP_HTTP_H

#include "core/io/tcp_server.h"
#include "core/io/stream_peer_tcp.h"
#include "core/string/ustring.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class McpServer;

// Push an SSE frame to a connection owned by MCPHttpServer (from any thread).
void mcp_http_send_frame(void *p_conn, const String &p_frame);

class MCPHttpServer {
public:
	MCPHttpServer(McpServer *p_owner);
	~MCPHttpServer();

	struct Connection;

	bool start();
	void stop();
	int get_port() const { return port; }

private:

	void _accept_loop();
	void _connection_loop(Connection *p_conn);
	bool _read_request(Connection *p_conn);
	void _handle_http(Connection *p_conn);
	void _send_response(Connection *p_conn, int p_status, const String &p_content_type, const String &p_body, const String &p_extra_headers);
	void _write_sse(Connection *p_conn, const String &p_frame);
	bool _check_auth(Connection *p_conn);
	void _close_conn(Connection *p_conn);

	McpServer *owner;
	Ref<TCPServer> server;
	int port = 8766;
	String bind = "0.0.0.0";
	std::atomic<bool> stopping{ false };

	std::unique_ptr<std::thread> accept_thread;
	std::vector<std::unique_ptr<std::thread>> conn_threads;
	std::mutex conn_threads_mu;
};

#endif // GODOT_MCP_HTTP_H