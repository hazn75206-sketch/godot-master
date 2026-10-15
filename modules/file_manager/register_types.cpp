#include "register_types.h"

#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "fm_dock.h"
#endif

void initialize_file_manager_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(McpFileTree);
		GDREGISTER_CLASS(McpFileManager);
		GDREGISTER_CLASS(McpFileManagerPlugin);
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorNode::add_init_callback([]() {
			EditorNode::get_singleton()->add_editor_plugin(memnew(McpFileManagerPlugin));
		});
	}
#endif
}

void uninitialize_file_manager_module(ModuleInitializationLevel p_level) {
}
