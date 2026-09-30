#ifndef GODOT_AGENT_CHAT_H
#define GODOT_AGENT_CHAT_H

#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/string/ustring.h"

// Agent chat tiruan-opencode di dalam editor (1 aplikasi, tanpa binari/Termux).
// Provider: OpenAI-compatible HTTP (Zen default + custom base_url/key/model).
// Tools: 50 tools MCP dijalankan in-process. Riwayat: user://agent_sessions/.
void agent_chat_register_settings();

// Buat session baru, kembalikan id-nya.
String agent_chat_new_session();

// Kirim prompt (blocking! panggil dari worker thread).
// Kembali: { text, tools_used:Array[String], session, error }.
Dictionary agent_chat_send(const String &p_session_id, const String &p_prompt);

// Minta pembatalan request yang sedang berjalan (dipanggil dari UI thread).
void agent_chat_cancel();

// Ambil daftar model dari provider (blocking! panggil dari worker thread).
// Kembali: { models:Array[String], error }.
Dictionary agent_chat_fetch_models();

#endif // GODOT_AGENT_CHAT_H
