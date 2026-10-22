#ifndef GODOT_MCP_TOOLS_H
#define GODOT_MCP_TOOLS_H

#include "core/variant/variant.h"
#include "core/variant/array.h"
#include "core/string/ustring.h"

#include <cstdint>

class McpServer;

void mcp_register_tools(McpServer *p_server);

void mcp_log_append(const String &p_text, bool p_error = false, bool p_warning = false, const String &p_file = String(), int p_line = 0, const String &p_func = String());
uint64_t mcp_log_error_seq();
Array mcp_log_errors_since(uint64_t p_seq);

Variant mcp_tool_ret_text(const String &p_text);
Variant mcp_tool_ret_error(const String &p_text);
Variant mcp_tool_ret_json(const Variant &p_value);

#endif // GODOT_MCP_TOOLS_H