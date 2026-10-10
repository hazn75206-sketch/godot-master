#include "register_types.h"

#include "core/object/class_db.h"
#include "mcp_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "fm_dock.h"
#endif

void initialize_godot_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(McpServer);
#ifdef TOOLS_ENABLED
		GDREGISTER_CLASS(McpFileTree);
		GDREGISTER_CLASS(McpFileManager);
		GDREGISTER_CLASS(McpFileManagerPlugin);
#endif
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback([]() {
			EditorNode::get_singleton()->add_editor_plugin(memnew(McpFileManagerPlugin));
		});
	}
#endif
}

void uninitialize_godot_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	McpServer::get_singleton()->stop_server();
	McpServer::cleanup();
}
