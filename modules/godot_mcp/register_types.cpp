#include "register_types.h"

#include "core/object/class_db.h"
#include "mcp_server.h"

void initialize_godot_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(McpServer);
}

void uninitialize_godot_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	McpServer::get_singleton()->stop_server();
	McpServer::cleanup();
}
