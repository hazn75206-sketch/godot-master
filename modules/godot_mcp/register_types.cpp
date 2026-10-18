#include "register_types.h"

#include "core/object/class_db.h"
#include "mcp_server.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "term_dock.h"
#endif

void initialize_godot_mcp_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(McpServer);
#ifdef TOOLS_ENABLED
		GDREGISTER_CLASS(TermView);
		GDREGISTER_CLASS(TermDock);
		GDREGISTER_CLASS(TermDockPlugin);
#endif
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback([]() {
			TermDockPlugin *plugin = memnew(TermDockPlugin);
			plugin->setup();
			EditorNode::get_singleton()->add_editor_plugin(plugin);
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
