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
#include "scene/gui/texture_rect.h"

// ItemList yang bisa di-seret keluar sebagai file (format drop standar
// FileSystem dock + flag fm_copy agar selalu COPY, bukan MOVE).
class McpFileList : public ItemList {
	GDCLASS(McpFileList, ItemList);

public:
	virtual Variant get_drag_data(const Point2 &p_point) override;
	McpFileList() {}
};

class McpFileManager : public PanelContainer {
	GDCLASS(McpFileManager, PanelContainer);

	Vector<String> shortcuts;

	HBoxContainer *crumb_bar = nullptr;
	LineEdit *search_box = nullptr;
	OptionButton *sort_box = nullptr;
	HBoxContainer *shortcut_bar = nullptr;
	McpFileList *list = nullptr;
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

	// Button refs untuk _apply_button_icons
	Button *btn_back = nullptr;
	Button *btn_home = nullptr;
	Button *btn_refresh = nullptr;
	Button *btn_copy = nullptr;
	Button *btn_cut = nullptr;
	Button *btn_paste = nullptr;
	Button *btn_rename = nullptr;
	Button *btn_delete = nullptr;
	Button *btn_mkdir = nullptr;
	Button *btn_extract = nullptr;
	Button *btn_import = nullptr;
	Button *btn_info = nullptr;

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
	void _do_delete(const Vector<String> &p_src);
	void _do_rename(const String &p_src, const String &p_new_name);
	void _do_mkdir(const String &p_name);
	void _do_extract(const String &p_zip);
	void _do_extract_zip(const String &p_zip);
	void _do_extract_tar(const String &p_path, bool p_gzipped);
	void _do_import(const Vector<String> &p_src, const String &p_dst_dir);
	void _show_info(const Vector<String> &p_src);
	void _scan_if_inside_project(const String &p_path);
	Error _copy_recursive(const String &p_from, const String &p_to, int &r_count);
	Error _remove_recursive(const String &p_path);

	void _on_item_activated(int p_idx);
	void _on_search_changed(const String &p_text);
	void _on_sort_changed(int p_idx);
	void _on_copy();
	void _on_cut();
	void _on_paste();
	void _on_rename();
	void _on_delete();
	void _on_mkdir();
	void _on_extract();
	void _on_import();
	void _on_info();
	void _on_confirm();
	void _on_input_confirm();
	void _show_preview(const String &p_path);
	void _on_preview_zoom(float p_factor);
	bool _compare(const Variant &p_a, const Variant &p_b, int p_mode);
	void _apply_button_icons();

	AcceptDialog *preview_dlg = nullptr;
	TextureRect *preview_tr = nullptr;
	Label *preview_zoom_label = nullptr;
	Vector2 preview_base = Vector2(0, 0);
	float preview_zoom = 1.0f;

protected:
	static void _bind_methods();
	void _enter_tree();
	void _exit_tree();

public:
	void open_dir(const String &p_dir);
	String get_current_dir() const { return current_dir; }
	McpFileManager();
};

class McpFileManagerPlugin : public EditorPlugin {
	GDCLASS(McpFileManagerPlugin, EditorPlugin);

	Window *win = nullptr;
	McpFileManager *fm = nullptr;
	Button *toolbar_btn = nullptr;

	void _open_manager();
	void refresh_toolbar_icon();

protected:
	void _enter_tree();
	void _exit_tree();
	void _unhandled_key_input(const Ref<InputEvent> &p_event);

public:
	static McpFileManagerPlugin *get_singleton() { return singleton; }
	static void open_file_manager() { if (singleton) singleton->_open_manager(); }
	McpFileManagerPlugin();
	~McpFileManagerPlugin();
private:
	static McpFileManagerPlugin *singleton;
};

#endif // TOOLS_ENABLED

#endif // FM_DOCK_H
