#ifndef FM_DOCK_H
#define FM_DOCK_H

#ifdef TOOLS_ENABLED

#include "editor/plugins/editor_plugin.h"
#include "core/input/input_event.h"
#include "core/math/vector2.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/item_list.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/tree.h"
#include "core/templates/vector.h"

// Tree 4 kolom yang bisa di-seret keluar sebagai file (format drop standar
// FileSystem dock + flag fm_copy agar selalu COPY, bukan MOVE).
// Preview bawaan engine di-clip oleh jendela asal; overlay di viewport editor
// dipakai supaya selama seret lintas jendela preview tetap terlihat.
class McpFileTree : public Tree {
	GDCLASS(McpFileTree, Tree);

	ObjectID screen_preview_id;

	void _begin_screen_preview(int p_count);
	void _update_screen_preview();
	void _clear_screen_preview();

protected:
	void _notification(int p_what);

public:
	virtual Variant get_drag_data(const Point2 &p_point) override;
	McpFileTree() {}
};

class McpFileManager : public PanelContainer {
	GDCLASS(McpFileManager, PanelContainer);

	Vector<String> shortcuts;

	HBoxContainer *crumb_bar = nullptr;
	LineEdit *search_box = nullptr;
	OptionButton *sort_box = nullptr;
	HBoxContainer *shortcut_bar = nullptr;
	McpFileTree *list = nullptr;
	Label *status_label = nullptr;

	AcceptDialog *confirm_dialog = nullptr;
	String confirm_action;
	AcceptDialog *input_dialog = nullptr;
	LineEdit *input_edit = nullptr;
	String input_action;

	Vector<String> clipboard;
	bool clipboard_cut = false;

	String current_dir;
	int sort_mode = 0;
	bool ui_built = false;

	// Button refs untuk _apply_button_icons
	Button *btn_back = nullptr;
	Button *btn_home = nullptr;
	Button *btn_refresh = nullptr;
	Vector<Button *> action_buttons;
	Vector<String> action_icon_names;

	void _build_ui();
	void _build_shortcuts();
	void _refresh();
	void _go_to(const String &p_dir);
	void _go_up();
	void _on_home();
	void _add_crumb(const String &p_label, const String &p_path);
	Vector<String> _selected_paths() const;
	String _fmt_size(uint64_t p_bytes) const;
	String _icon_for(const String &p_name, bool p_dir) const;
	void _do_copy(const Vector<String> &p_src, const String &p_dst_dir);
	bool _do_move(const Vector<String> &p_src, const String &p_dst_dir);
	void _reimport_copied(const Vector<String> &p_paths);
	void _do_delete(const Vector<String> &p_src);
	void _do_rename(const String &p_src, const String &p_new_name);
	void _do_mkdir(const String &p_name);
	void _do_extract(const String &p_zip);
	void _do_extract_zip(const String &p_zip);
	void _do_extract_tar(const String &p_path, bool p_gzipped);
	void _show_info(const Vector<String> &p_src);
	void _scan_if_inside_project(const String &p_path);
	Error _copy_recursive(const String &p_from, const String &p_to, int &r_count);
	Error _remove_recursive(const String &p_path);

	void _on_item_activated();
	void _open_path(const String &p_path);
	void _on_open_selected();
	Button *_make_action(const char *p_icon, const char *p_label, const char *p_tip, void (McpFileManager::*p_method)());
	void _on_search_changed(const String &p_text);
	void _on_sort_changed(int p_idx);
	void _on_copy();
	void _on_cut();
	void _on_paste();
	void _on_rename();
	void _on_delete();
	void _on_mkdir();
	void _on_extract();
	void _on_info();
	void _on_confirm();
	void _on_input_confirm();
	bool _compare(const Variant &p_a, const Variant &p_b, int p_mode);
	void _apply_button_icons();


protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void open_dir(const String &p_dir);
	String get_current_dir() const { return current_dir; }
	McpFileManager();
};

class McpFileManagerPlugin : public EditorPlugin {
	GDCLASS(McpFileManagerPlugin, EditorPlugin);

	Window *win = nullptr;
	McpFileManager *fm = nullptr;

	void _open_manager();

public:
	// Registrasi editor dipisah dari constructor: ClassDB meng-instansiasi
	// kelas ini saat generate dokumentasi (juga di Project Manager tanpa
	// EditorNode), jadi constructor wajib bebas efek samping.
	void setup();
	static McpFileManagerPlugin *get_singleton() { return singleton; }
	static void open_file_manager() {
		// The Inspector dock can outlive/rebuild editor plugins. Resolve a live
		// instance at click time instead of silently relying on a stale singleton.
		for (int i = instances.size() - 1; i >= 0; i--) {
			McpFileManagerPlugin *plugin = instances[i];
			if (plugin && plugin->is_inside_tree()) {
				singleton = plugin;
				plugin->_open_manager();
				return;
			}
		}
		WARN_PRINT("File Manager: no active plugin instance found for Inspector button.");
	}
	McpFileManagerPlugin();
	~McpFileManagerPlugin();

protected:
	void unhandled_key_input(const Ref<InputEvent> &p_event) override;

private:
	static McpFileManagerPlugin *singleton;
	static Vector<McpFileManagerPlugin *> instances;
};

#endif // TOOLS_ENABLED

#endif // FM_DOCK_H
