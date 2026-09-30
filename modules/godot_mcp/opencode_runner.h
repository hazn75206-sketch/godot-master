#ifndef GODOT_OPENCODE_RUNNER_H
#define GODOT_OPENCODE_RUNNER_H

#include "core/variant/dictionary.h"
#include "core/string/string.h"

// Bundled on-device opencode binary (see platform/android/.../opencode/OpencodeRunner.java).
// Settings: opencode/model, opencode/mcp_url (Editor Settings).
void opencode_runner_register_settings();

// Blocking status query: { installed, version, model, mcp_url }.
Dictionary opencode_runner_status();

// Blocking `opencode run`: { output, error }. Call from a worker thread,
// never from the main thread (LLM calls take a long time).
Dictionary opencode_runner_run(const String &p_prompt);

#endif // GODOT_OPENCODE_RUNNER_H
