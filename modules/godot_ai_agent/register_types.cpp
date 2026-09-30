#include "register_types.h"

#include "agent_chat.h"
#include "core/object/class_db.h"
#include "opencode_dock_plugin.h"

void initialize_godot_ai_agent_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
#ifdef TOOLS_ENABLED
	GDREGISTER_CLASS(OpencodeDockPlugin);
	agent_chat_register_settings();
#endif
}

void uninitialize_godot_ai_agent_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
