#ifdef TOOLS_ENABLED

#include "fm_dock.h"

#include "core/input/input_event.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/zip_io.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_toaster.h"
#include "editor/gui/progress_dialog.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/option_button.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/window.h"
#include "scene/gui/texture_rect.h"
#include "scene/resources/image_texture.h"
#include "servers/display/display_server.h"

// DirAccess::make_dir_recursive is an instance method in this Godot version.
static Error fm_make_dir_recursive(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (dir.is_null()) {
		return ERR_CANT_CREATE;
	}
	return dir->make_dir_recursive(p_path);
}

// ---------------------------------------------------------------- McpFileList

Variant McpFileList::get_drag_data(const Point2 &p_point) {
	int idx = get_item_at_position(p_point, true);
	if (idx < 0 || !is_selected(idx)) {
		return Variant();
	}
	Vector<int> sel = get_selected_items();
	if (sel.is_empty()) {
		return Variant();
	}
	Array files;
	for (int i : sel) {
		files.append(get_item_metadata(i));
	}
	Dictionary dd;
	dd["type"] = "files_and_dirs";
	dd["files"] = files;
	dd["fm_copy"] = true; // Selalu COPY: file asli di storage tidak boleh hilang.
	Label *preview = memnew(Label);
	preview->set_text(vformat("%d item (salin)", files.size()));
	set_drag_preview(preview);
	return dd;
}

// -------------------------------------------------------------- McpFileManager

McpFileManager::McpFileManager() {
	_build_ui();
	shortcuts.push_back("/storage/emulated/0/Documents");
	shortcuts.push_back("/storage/emulated/0/DCIM");
	shortcuts.push_back("/storage/emulated/0/Download");
	shortcuts.push_back(ProjectSettings::get_singleton()->globalize_path("res://"));
	_build_shortcuts();
	open_dir("/storage/emulated/0/Documents");
}

void McpFileManager::_build_ui() {
	VBoxContainer *root = memnew(VBoxContainer);
	add_child(root);

	HBoxContainer *nav = memnew(HBoxContainer);
	root->add_child(nav);
	btn_back = memnew(Button);
	btn_back->set_tooltip_text("Kembali");
	btn_back->connect("pressed", callable_mp(this, &McpFileManager::_go_up));
	nav->add_child(btn_back);
	btn_back->set_name("NavBack");
	btn_home = memnew(Button);
	btn_home->set_tooltip_text("Proyek aktif");
	btn_home->connect("pressed", callable_mp(this, &McpFileManager::_on_home));
	nav->add_child(btn_home);
	btn_home->set_name("NavHome");
	btn_refresh = memnew(Button);
	btn_refresh->set_tooltip_text("Segarkan");
	btn_refresh->connect("pressed", callable_mp(this, &McpFileManager::_refresh));
	nav->add_child(btn_refresh);
	btn_refresh->set_name("NavRefresh");
	ScrollContainer *crumb_scroll = memnew(ScrollContainer);
	crumb_scroll->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
	crumb_scroll->set_h_scroll(true);
	crumb_scroll->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	nav->add_child(crumb_scroll);
	crumb_bar = memnew(HBoxContainer);
	crumb_scroll->add_child(crumb_bar);

	search_box = memnew(LineEdit);
	search_box->set_placeholder("Cari...");
	search_box->set_clear_button_enabled(true);
	search_box->set_custom_minimum_size(Vector2(110, 0));
	search_box->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	search_box->connect("text_changed", callable_mp(this, &McpFileManager::_on_search_changed));
	nav->add_child(search_box);

	sort_box = memnew(OptionButton);
	sort_box->add_item("Nama", 0);
	sort_box->add_item("Ukuran", 1);
	sort_box->add_item("Tanggal", 2);
	sort_box->connect("item_selected", callable_mp(this, &McpFileManager::_on_sort_changed));
	nav->add_child(sort_box);

	shortcut_bar = memnew(HBoxContainer);
	root->add_child(shortcut_bar);

	list = memnew(McpFileList);
	list->set_select_mode(ItemList::SELECT_MULTI);
	list->set_allow_rmb_select(true);
	list->set_fixed_icon_size(Vector2i(20, 20));
	list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	list->connect("item_activated", callable_mp(this, &McpFileManager::_on_item_activated));
	root->add_child(list);

	HBoxContainer *actions = memnew(HBoxContainer);
	actions->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	root->add_child(actions);
	const char *btns[][2] = {
		{ "ActCopy", "Salin" }, { "ActCut", "Potong" }, { "ActPaste", "Tempel" },
		{ "ActRename", "Ganti nama" }, { "ActDelete", "Hapus" }, { "ActMkdir", "Folder" },
		{ "ActExtract", "Ekstrak" }, { "ActImport", "Import" }, { "ActInfo", "Info" },
	};
	for (int i = 0; i < 9; i++) {
		Button *btn = memnew(Button);
		btn->set_tooltip_text(btns[i][1]);
		btn->set_name(btns[i][0]);
		actions->add_child(btn);
	}
	btn_copy = Object::cast_to<Button>(actions->get_child(0));
	btn_cut = Object::cast_to<Button>(actions->get_child(1));
	btn_paste = Object::cast_to<Button>(actions->get_child(2));
	btn_rename = Object::cast_to<Button>(actions->get_child(3));
	btn_delete = Object::cast_to<Button>(actions->get_child(4));
	btn_mkdir = Object::cast_to<Button>(actions->get_child(5));
	btn_extract = Object::cast_to<Button>(actions->get_child(6));
	btn_import = Object::cast_to<Button>(actions->get_child(7));
	btn_info = Object::cast_to<Button>(actions->get_child(8));

	btn_copy->connect("pressed", callable_mp(this, &McpFileManager::_on_copy));
	btn_cut->connect("pressed", callable_mp(this, &McpFileManager::_on_cut));
	btn_paste->connect("pressed", callable_mp(this, &McpFileManager::_on_paste));
	btn_rename->connect("pressed", callable_mp(this, &McpFileManager::_on_rename));
	btn_delete->connect("pressed", callable_mp(this, &McpFileManager::_on_delete));
	btn_mkdir->connect("pressed", callable_mp(this, &McpFileManager::_on_mkdir));
	btn_extract->connect("pressed", callable_mp(this, &McpFileManager::_on_extract));
	btn_import->connect("pressed", callable_mp(this, &McpFileManager::_on_import));
	btn_info->connect("pressed", callable_mp(this, &McpFileManager::_on_info));

	status_label = memnew(Label);
	status_label->set_clip_text(true);
	root->add_child(status_label);

	confirm_dialog = memnew(AcceptDialog);
	confirm_dialog->connect("confirmed", callable_mp(this, &McpFileManager::_on_confirm));
	add_child(confirm_dialog);

	input_dialog = memnew(AcceptDialog);
	input_edit = memnew(LineEdit);
	input_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	input_dialog->add_child(input_edit);
	input_dialog->connect("confirmed", callable_mp(this, &McpFileManager::_on_input_confirm));
	add_child(input_dialog);
}

void McpFileManager::_build_shortcuts() {
	// Dibangun ulang tiap refresh shortcut (dipanggil sekali dari konstruktor).
	for (int i = shortcut_bar->get_child_count() - 1; i >= 0; i--) {
		shortcut_bar->get_child(i)->queue_free();
	}
	for (const String &s : shortcuts) {
		Button *b = memnew(Button);
		b->set_text(s.get_file().is_empty() ? s : s.get_file());
		b->set_tooltip_text(s);
		b->connect("pressed", callable_mp(this, &McpFileManager::_go_to).bind(s));
		shortcut_bar->add_child(b);
	}
}

void McpFileManager::open_dir(const String &p_dir) {
	_go_to(p_dir);
}

void McpFileManager::_go_to(const String &p_dir) {
	if (!DirAccess::exists(p_dir)) {
		status_label->set_text("Folder tidak ada: " + p_dir);
		return;
	}
	current_dir = p_dir;
	_refresh();
}

void McpFileManager::_go_up() {
	if (current_dir == "/" || current_dir.is_empty()) {
		return;
	}
	_go_to(current_dir.get_base_dir());
}

void McpFileManager::_add_crumb(const String &p_label, const String &p_path) {
	Button *b = memnew(Button);
	b->set_text(p_label);
	b->set_flat(true);
	b->connect("pressed", callable_mp(this, &McpFileManager::_go_to).bind(p_path));
	crumb_bar->add_child(b);
}

void McpFileManager::_refresh() {
	for (int i = crumb_bar->get_child_count() - 1; i >= 0; i--) {
		crumb_bar->get_child(i)->queue_free();
	}
	_add_crumb("/", "/");
	String acc;
	for (const String &part : current_dir.trim_prefix("/").split("/")) {
		if (part.is_empty()) {
			continue;
		}
		acc += "/" + part;
		_add_crumb(part, acc);
	}
	// Icon tombol nav + aksi (theme EditorIcons, hasil build modul).
	list->clear();
	Ref<DirAccess> d = DirAccess::open(current_dir);
	if (d.is_null()) {
		status_label->set_text("Tidak bisa dibuka (izin?): " + current_dir);
		return;
	}
	String filter = search_box->get_text().to_lower();
	Array rows;
	d->list_dir_begin();
	String fn = d->get_next();
	while (!fn.is_empty()) {
		if (!filter.is_empty() && fn.to_lower().find(filter) < 0) {
			fn = d->get_next();
			continue;
		}
		Dictionary r;
		r["name"] = fn;
		r["dir"] = d->current_is_dir();
		String full = current_dir.rstrip("/") + "/" + fn;
		r["path"] = full;
		if (bool(r["dir"])) {
			r["size"] = uint64_t(0);
			r["mtime"] = uint64_t(0);
		} else {
			Ref<FileAccess> f = FileAccess::open(full, FileAccess::READ);
			r["size"] = f.is_valid() ? f->get_length() : uint64_t(0);
			r["mtime"] = FileAccess::get_modified_time(full);
		}
		rows.append(r);
		fn = d->get_next();
	}
	d->list_dir_end();
	rows.sort_custom(callable_mp(this, &McpFileManager::_compare).bind(sort_mode));
	for (int i = 0; i < rows.size(); i++) {
		Dictionary r = rows[i];
		String label = String(r["name"]);
		if (!bool(r["dir"])) {
			label += "  (" + _fmt_size(uint64_t(r["size"])) + ")";
		}
		int idx = list->add_item(label, get_theme_icon(_icon_for(String(r["name"]), bool(r["dir"])), "EditorIcons"));
		list->set_item_metadata(idx, String(r["path"]));
		list->set_item_tooltip(idx, String(r["path"]));
		// Feedback visual untuk item yang di-cut: redupkan.
		if (clipboard_cut && clipboard.has(String(r["path"]))) {
			list->set_item_custom_fg_color(idx, Color(1, 1, 1, 0.45));
		}
	}
	status_label->set_text(vformat("%s — %d item", current_dir, rows.size()));
	_apply_button_icons();
}

bool McpFileManager::_compare(const Variant &p_a, const Variant &p_b, int p_mode) {
	Dictionary a = p_a;
	Dictionary b = p_b;
	bool ad = bool(a["dir"]);
	bool bd = bool(b["dir"]);
	if (ad != bd) {
		return ad > bd; // Folder dulu.
	}
	if (p_mode == 1) {
		uint64_t sa = uint64_t(a["size"]);
		uint64_t sb = uint64_t(b["size"]);
		if (sa != sb) {
			return sa < sb;
		}
	} else if (p_mode == 2) {
		uint64_t ma = uint64_t(a["mtime"]);
		uint64_t mb = uint64_t(b["mtime"]);
		if (ma != mb) {
			return ma > mb;
		}
	}
	return String(a["name"]).naturalnocasecmp_to(String(b["name"])) < 0;
}

String McpFileManager::_fmt_size(uint64_t p_bytes) const {
	const char *u[] = { "B", "KB", "MB", "GB" };
	double v = (double)p_bytes;
	int i = 0;
	while (v >= 1024.0 && i < 3) {
		v /= 1024.0;
		i++;
	}
	return vformat(i == 0 ? "%d %s" : "%.1f %s", i == 0 ? uint64_t(v) : v, u[i]);
}

String McpFileManager::_icon_for(const String &p_name, bool p_dir) const {
	if (p_dir) {
		return "fm_dir";
	}
	String ext = p_name.get_extension().to_lower();
	if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg" || ext == "bmp" || ext == "gif") {
		return "fm_image";
	}
	if (ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "webm" || ext == "avi") {
		return "fm_video";
	}
	if (ext == "mp3" || ext == "ogg" || ext == "wav" || ext == "flac" || ext == "m4a") {
		return "fm_audio";
	}
	if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" || ext == "gz" || ext == "apk") {
		return ext == "apk" ? "fm_apk" : "fm_archive";
	}
	if (ext == "txt" || ext == "md" || ext == "log" || ext == "json" || ext == "xml" || ext == "csv") {
		return "fm_text";
	}
	if (ext == "gd" || ext == "cpp" || ext == "h" || ext == "hpp" || ext == "c" || ext == "py" || ext == "js" || ext == "ts" || ext == "cs" || ext == "kt" || ext == "java" || ext == "glsl" || ext == "gdshader" || ext == "tscn" || ext == "tres" || ext == "godot") {
		return "fm_doc";
	}
	return "fm_doc";
}

void McpFileManager::_apply_button_icons() {
	// Nav buttons
	if (btn_back) btn_back->set_button_icon(get_theme_icon("fm_back", "EditorIcons"));
	if (btn_home) btn_home->set_button_icon(get_theme_icon("fm_home", "EditorIcons"));
	if (btn_refresh) btn_refresh->set_button_icon(get_theme_icon("fm_refresh", "EditorIcons"));

	// Action buttons
	if (btn_copy) btn_copy->set_button_icon(get_theme_icon("fm_copy", "EditorIcons"));
	if (btn_cut) btn_cut->set_button_icon(get_theme_icon("fm_cut", "EditorIcons"));
	if (btn_paste) btn_paste->set_button_icon(get_theme_icon("fm_paste", "EditorIcons"));
	if (btn_rename) btn_rename->set_button_icon(get_theme_icon("fm_rename", "EditorIcons"));
	if (btn_delete) btn_delete->set_button_icon(get_theme_icon("fm_delete", "EditorIcons"));
	if (btn_mkdir) btn_mkdir->set_button_icon(get_theme_icon("fm_newfolder", "EditorIcons"));
	if (btn_extract) btn_extract->set_button_icon(get_theme_icon("fm_extract", "EditorIcons"));
	if (btn_import) btn_import->set_button_icon(get_theme_icon("fm_import", "EditorIcons"));
	if (btn_info) btn_info->set_button_icon(get_theme_icon("fm_info", "EditorIcons"));
}

Vector<String> McpFileManager::_selected_paths() const {
	Vector<String> out;
	for (int i : list->get_selected_items()) {
		out.append(String(list->get_item_metadata(i)));
	}
	return out;
}

void McpFileManager::_scan_if_inside_project(const String &p_path) {
	String proj = ProjectSettings::get_singleton()->globalize_path("res://");
	if (p_path.begins_with(proj)) {
		EditorFileSystem *efs = EditorFileSystem::get_singleton();
		if (efs) {
			efs->scan_changes();
		}
	}
}

void McpFileManager::_on_item_activated(int p_idx) {
	String path = String(list->get_item_metadata(p_idx));
	if (DirAccess::exists(path)) {
		_go_to(path);
		return;
	}
	String ext = path.get_extension().to_lower();
	if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "bmp" || ext == "gif") {
		_show_preview(path);
		return;
	}
	if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "tgz" || ext == "rar") {
		_do_extract(path);
		return;
	}
	OS::get_singleton()->shell_open(path);
}

void McpFileManager::_on_search_changed(const String &p_text) {
	(void)p_text;
	_refresh();
}

void McpFileManager::_on_sort_changed(int p_idx) {
	sort_mode = p_idx;
	_refresh();
}

void McpFileManager::_on_home() {
	_go_to(ProjectSettings::get_singleton()->globalize_path("res://"));
}

void McpFileManager::_on_copy() {
	clipboard = _selected_paths();
	clipboard_cut = false;
	status_label->set_text(vformat("%d item disalin ke clipboard", clipboard.size()));
}

void McpFileManager::_on_cut() {
	clipboard = _selected_paths();
	clipboard_cut = true;
	status_label->set_text(vformat("%d item dipotong ke clipboard", clipboard.size()));
}

void McpFileManager::_on_paste() {
	if (clipboard.is_empty()) {
		return;
	}
	if (clipboard_cut) {
		// Simpan copy clipboard dulu, baru clear kalau move berhasil
		Vector<String> to_move = clipboard;
		if (_do_move(to_move, current_dir)) {
			clipboard.clear();
			clipboard_cut = false;
		}
	} else {
		_do_copy(clipboard, current_dir);
	}
	_refresh();
	_scan_if_inside_project(current_dir);
}

void McpFileManager::_on_rename() {
	Vector<String> sel = _selected_paths();
	if (sel.is_empty()) {
		status_label->set_text("Tidak ada item yang dipilih.");
		return;
	}
	if (sel.size() != 1) {
		status_label->set_text("Pilih tepat 1 item untuk ganti nama.");
		return;
	}
	// Gunakan delimiter | yang jarang ada di nama file
	input_action = "rename|" + sel[0];
	input_edit->set_text(sel[0].get_file());
	input_dialog->set_title("Ganti nama");
	input_dialog->popup_centered(Vector2i(420, 140));
	input_edit->grab_focus();
	input_edit->select_all();
}

void McpFileManager::_on_delete() {
	Vector<String> sel = _selected_paths();
	if (sel.is_empty()) {
		return;
	}
	confirm_action = "delete";
	confirm_dialog->set_text(vformat("Hapus %d item? Tidak bisa dibatalkan.", sel.size()));
	confirm_dialog->set_title("Hapus");
	confirm_dialog->popup_centered();
}

void McpFileManager::_on_mkdir() {
	input_action = "mkdir";
	input_edit->set_text("Folder baru");
	input_dialog->set_title("Folder baru");
	input_dialog->popup_centered(Vector2i(420, 140));
	input_edit->grab_focus();
	input_edit->select_all();
}

void McpFileManager::_on_extract() {
	Vector<String> sel = _selected_paths();
	if (sel.size() != 1) {
		status_label->set_text("Pilih tepat 1 file arsip.");
		return;
	}
	String ext = sel[0].get_extension().to_lower();
	if (ext != "zip" && ext != "tar" && ext != "gz" && ext != "tgz" && ext != "rar") {
		status_label->set_text("Bukan file arsip (zip/tar/gz/tgz/rar).");
		return;
	}
	_do_extract(sel[0]);
}

void McpFileManager::_show_preview(const String &p_path) {
	Ref<Image> img;
	img.instantiate();
	if (img->load(p_path) != OK) {
		status_label->set_text("Gagal membuka gambar.");
		return;
	}
	preview_base = Vector2(img->get_width(), img->get_height());
	preview_zoom = 1.0f;
	if (preview_dlg == nullptr) {
		preview_dlg = memnew(AcceptDialog);
		preview_dlg->set_title(p_path.get_file());
		VBoxContainer *vb = memnew(VBoxContainer);
		preview_dlg->add_child(vb);
		HBoxContainer *zb = memnew(HBoxContainer);
		zb->set_alignment(BoxContainer::ALIGNMENT_CENTER);
		vb->add_child(zb);
		Button *zin = memnew(Button);
		zin->set_text("+");
		zin->connect("pressed", callable_mp(this, &McpFileManager::_on_preview_zoom).bind(1.25f));
		zb->add_child(zin);
		preview_zoom_label = memnew(Label);
		preview_zoom_label->set_text("100%");
		zb->add_child(preview_zoom_label);
		Button *zout = memnew(Button);
		zout->set_text("-");
		zout->connect("pressed", callable_mp(this, &McpFileManager::_on_preview_zoom).bind(0.8f));
		zb->add_child(zout);
		preview_tr = memnew(TextureRect);
		preview_tr->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		preview_tr->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		vb->add_child(preview_tr);
		add_child(preview_dlg);
	}
	Ref<ImageTexture> tex;
	tex.instantiate();
	tex->set_image(img);
	preview_tr->set_texture(tex);
	_on_preview_zoom(1.0f);
	preview_dlg->set_title(p_path.get_file());
	preview_dlg->popup_centered();
}

void McpFileManager::_on_preview_zoom(float p_factor) {
	preview_zoom = CLAMP(preview_zoom * p_factor, 0.1f, 8.0f);
	if (preview_tr) {
		preview_tr->set_custom_minimum_size(preview_base * preview_zoom);
	}
	if (preview_zoom_label) {
		preview_zoom_label->set_text(vformat("%d%%", int(preview_zoom * 100.0f)));
	}
}

void McpFileManager::_on_import() {
	Vector<String> sel = _selected_paths();
	if (sel.is_empty()) {
		return;
	}
	input_action = "import";
	input_edit->set_text("res://fm_import/");
	input_dialog->set_title("Import ke proyek (folder tujuan)");
	input_dialog->popup_centered(Vector2i(460, 140));
	input_edit->grab_focus();
}

void McpFileManager::_on_info() {
	_show_info(_selected_paths());
}

void McpFileManager::_on_confirm() {
	if (confirm_action == "delete") {
		_do_delete(_selected_paths());
		_refresh();
		_scan_if_inside_project(current_dir);
	}
	confirm_action = "";
}

void McpFileManager::_on_input_confirm() {
	String v = input_edit->get_text().strip_edges();
	if (v.is_empty()) {
		return;
	}
	if (input_action.begins_with("rename|")) {
		_do_rename(input_action.trim_prefix("rename|"), v);
	} else if (input_action == "mkdir") {
		_do_mkdir(v);
	} else if (input_action == "import") {
		_do_import(_selected_paths(), v);
	}
	input_action = "";
	_refresh();
	_scan_if_inside_project(current_dir);
}

void McpFileManager::_do_copy(const Vector<String> &p_src, const String &p_dst_dir) {
	int count = 0;
	for (const String &s : p_src) {
		String dst = p_dst_dir.rstrip("/") + "/" + s.get_file();
		if (dst == s) {
			dst = p_dst_dir.rstrip("/") + "/" + s.get_basename() + "_salin." + s.get_extension();
		}
		if (_copy_recursive(s, dst, count) != OK) {
			status_label->set_text("Gagal menyalin: " + s);
			return;
		}
	}
	status_label->set_text(vformat("Disalin %d item.", count));
	EditorToaster::get_singleton()->popup_str(vformat("File manager: disalin %d item.", count));
}

bool McpFileManager::_do_move(const Vector<String> &p_src, const String &p_dst_dir) {
	for (const String &s : p_src) {
		String dst = p_dst_dir.rstrip("/") + "/" + s.get_file();
		if (dst == s) {
			continue;
		}
		if (DirAccess::rename_absolute(s, dst) != OK) {
			// Beda partisi: salin lalu hapus.
			int count = 0;
			if (_copy_recursive(s, dst, count) != OK || _remove_recursive(s) != OK) {
				status_label->set_text("Gagal memindah: " + s);
				return false;
			}
		}
	}
	status_label->set_text("Dipindah.");
	EditorToaster::get_singleton()->popup_str("File manager: dipindah.");
	return true;
}

void McpFileManager::_do_delete(const Vector<String> &p_src) {
	for (const String &s : p_src) {
		if (_remove_recursive(s) != OK) {
			status_label->set_text("Gagal menghapus: " + s);
			return;
		}
	}
	status_label->set_text("Dihapus.");
}

void McpFileManager::_do_rename(const String &p_src, const String &p_new_name) {
	String dst = p_src.get_base_dir().rstrip("/") + "/" + p_new_name;
	if (DirAccess::rename_absolute(p_src, dst) != OK) {
		status_label->set_text("Gagal ganti nama.");
	}
}

void McpFileManager::_do_mkdir(const String &p_name) {
	Ref<DirAccess> d = DirAccess::open(current_dir);
	if (d.is_null() || d->make_dir(p_name) != OK) {
		status_label->set_text("Gagal membuat folder.");
	}
}

Error McpFileManager::_copy_recursive(const String &p_from, const String &p_to, int &r_count) {
	if (DirAccess::exists(p_from)) {
		if (fm_make_dir_recursive(p_to) != OK) {
			return ERR_CANT_CREATE;
		}
		Ref<DirAccess> d = DirAccess::open(p_from);
		if (d.is_null()) {
			return ERR_CANT_OPEN;
		}
		d->list_dir_begin();
		String fn = d->get_next();
		while (!fn.is_empty()) {
			Error err = this->_copy_recursive(p_from.rstrip("/") + "/" + fn, p_to.rstrip("/") + "/" + fn, r_count);
			if (err != OK) {
				return err;
			}
			fn = d->get_next();
		}
		d->list_dir_end();
		return OK;
	}
	if (DirAccess::copy_absolute(p_from, p_to) != OK) {
		return ERR_CANT_CREATE;
	}
	r_count++;
	return OK;
}

Error McpFileManager::_remove_recursive(const String &p_path) {
	if (DirAccess::exists(p_path)) {
		Ref<DirAccess> d = DirAccess::open(p_path);
		if (d.is_null()) {
			return ERR_CANT_OPEN;
		}
		d->list_dir_begin();
		String fn = d->get_next();
		while (!fn.is_empty()) {
			Error err = this->_remove_recursive(p_path.rstrip("/") + "/" + fn);
			if (err != OK) {
				return err;
			}
			fn = d->get_next();
		}
		d->list_dir_end();
	}
	return DirAccess::remove_absolute(p_path);
}

// Dispatcher ekstrak: zip / tar / tar.gz+tgz native, rar ditolak baik-baik.
void McpFileManager::_do_extract(const String &p_path) {
	String ext = p_path.get_extension().to_lower();
	if (ext == "zip") {
		_do_extract_zip(p_path);
	} else if (ext == "tar") {
		_do_extract_tar(p_path, false);
	} else if (ext == "gz" || ext == "tgz") {
		_do_extract_tar(p_path, true);
	} else if (ext == "rar") {
		status_label->set_text("Format .rar tidak didukung. Gunakan tool eksternal.");
		EditorToaster::get_singleton()->popup_str("Format .rar tidak didukung di Godot.");
	} else {
		status_label->set_text("Format arsip tidak dikenali: " + ext);
	}
}

// Gunzip streaming ke file temp (hemat RAM untuk arsip besar).
static Error _fm_gunzip_to_temp(const String &p_src, const String &p_tmp) {
	z_stream zs = {};
	if (inflateInit2(&zs, 15 + 32) != Z_OK) {
		return ERR_CANT_OPEN;
	}
	Ref<FileAccess> fin = FileAccess::open(p_src, FileAccess::READ);
	if (fin.is_null()) {
		inflateEnd(&zs);
		return ERR_CANT_OPEN;
	}
	Ref<FileAccess> fout = FileAccess::open(p_tmp, FileAccess::WRITE);
	if (fout.is_null()) {
		inflateEnd(&zs);
		return ERR_CANT_CREATE;
	}
	const int CHUNK = 65536;
	Vector<uint8_t> inb;
	inb.resize(CHUNK);
	Vector<uint8_t> outb;
	outb.resize(CHUNK);
	bool stream_end = false;
	Error err = OK;
	while (!stream_end && fin->get_position() < (uint64_t)fin->get_length()) {
		uint64_t left = fin->get_length() - fin->get_position();
		int want = (int)MIN<uint64_t>(left, CHUNK);
		if (fin->get_buffer(inb.ptrw(), want) != want) {
			err = ERR_FILE_CORRUPT;
			break;
		}
		zs.next_in = (Bytef *)inb.ptr();
		zs.avail_in = want;
		do {
			zs.next_out = (Bytef *)outb.ptr();
			zs.avail_out = CHUNK;
			int ret = inflate(&zs, Z_NO_FLUSH);
			if (ret == Z_STREAM_ERROR || (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)) {
				err = ERR_FILE_CORRUPT;
				break;
			}
			int have = CHUNK - zs.avail_out;
			if (have > 0) {
				fout->store_buffer(outb.ptr(), have);
			}
			if (ret == Z_STREAM_END) {
				stream_end = true;
				break;
			}
			if (ret == Z_BUF_ERROR && zs.avail_in == 0) {
				break;
			}
		} while (zs.avail_out == 0);
		if (err != OK) {
			break;
		}
	}
	inflateEnd(&zs);
	return err;
}

// Parse header TAR USTAR: nama + ukuran (oktal). Return false kalau blok akhir.
static bool _fm_tar_header(const uint8_t *p_hdr, String &r_name, uint64_t &r_size) {
	bool empty = true;
	for (int i = 0; i < 512; i++) {
		if (p_hdr[i] != 0) {
			empty = false;
			break;
		}
	}
	if (empty) {
		return false;
	}
	char name_buf[101] = {};
	memcpy(name_buf, p_hdr, 100);
	r_name = String::utf8(name_buf).strip_edges();
	if (p_hdr[257] == 'u' && p_hdr[258] == 's' && p_hdr[259] == 't' && p_hdr[260] == 'a' && p_hdr[261] == 'r') {
		char pre_buf[156] = {};
		memcpy(pre_buf, p_hdr + 345, 155);
		String pre = String::utf8(pre_buf).strip_edges();
		if (!pre.is_empty() && !r_name.is_empty()) {
			r_name = pre + "/" + r_name;
		}
	}
	char size_buf[13] = {};
	memcpy(size_buf, p_hdr + 124, 12);
	r_size = strtoull(size_buf, nullptr, 8);
	return true;
}

void McpFileManager::_do_extract_tar(const String &p_path, bool p_gzipped) {
	String tar_path = p_path;
	String tmp_path;
	if (p_gzipped) {
		tmp_path = p_path.get_base_dir().rstrip("/") + "/.fm_tmp.tar";
		if (_fm_gunzip_to_temp(p_path, tmp_path) != OK) {
			status_label->set_text("Gagal decompress gzip.");
			DirAccess::remove_absolute(tmp_path);
			return;
		}
		tar_path = tmp_path;
	}
	Ref<FileAccess> f = FileAccess::open(tar_path, FileAccess::READ);
	if (f.is_null()) {
		status_label->set_text("Gagal membuka arsip tar.");
		if (!tmp_path.is_empty()) {
			DirAccess::remove_absolute(tmp_path);
		}
		return;
	}
	String dest = p_path.get_base_dir().rstrip("/") + "/" + p_path.get_file().get_basename().split(".")[0] + "/";
	fm_make_dir_recursive(dest);
	// Pass 1: hitung file untuk progress real.
	const int64_t TBLOCK = 512;
	Vector<uint8_t> hdr;
	hdr.resize(TBLOCK);
	int total = 0;
	while (f->get_position() + TBLOCK <= (uint64_t)f->get_length()) {
		if (f->get_buffer(hdr.ptrw(), TBLOCK) != TBLOCK) {
			break;
		}
		String nm;
		uint64_t sz = 0;
		if (!_fm_tar_header(hdr.ptr(), nm, sz)) {
			break;
		}
		if (!nm.ends_with("/") && nm.find("..") < 0) {
			total++;
		}
		int64_t skip = ((int64_t(sz) + TBLOCK - 1) / TBLOCK) * TBLOCK;
		f->seek(f->get_position() + skip);
	}
	f->seek(0);
	ProgressDialog::get_singleton()->add_task("fm_extract", "Mengekstrak tar", total);
	// Pass 2: tulis file streaming 64KB.
	const int CHUNK = 65536;
	Vector<uint8_t> chunk;
	chunk.resize(CHUNK);
	int done = 0;
	while (f->get_position() + TBLOCK <= (uint64_t)f->get_length()) {
		if (f->get_buffer(hdr.ptrw(), TBLOCK) != TBLOCK) {
			break;
		}
		String nm;
		uint64_t sz = 0;
		if (!_fm_tar_header(hdr.ptr(), nm, sz)) {
			break;
		}
		bool skip_entry = nm.ends_with("/") || nm.find("..") >= 0 || nm.is_absolute_path() || nm.is_empty();
		String out = dest + nm;
		Ref<FileAccess> fout;
		if (!skip_entry) {
			fm_make_dir_recursive(out.get_base_dir());
			fout = FileAccess::open(out, FileAccess::WRITE);
			if (fout.is_null()) {
				skip_entry = true;
			}
		}
		uint64_t left = sz;
		while (left > 0) {
			int want = (int)MIN<uint64_t>(left, CHUNK);
			int got = f->get_buffer(chunk.ptrw(), want);
			if (got <= 0) {
				break;
			}
			if (!skip_entry && fout.is_valid()) {
				fout->store_buffer(chunk.ptr(), got);
			}
			left -= got;
		}
		int64_t pad = (((int64_t(sz) + TBLOCK - 1) / TBLOCK) * TBLOCK) - (int64_t)sz;
		if (pad > 0) {
			f->seek(f->get_position() + pad);
		}
		if (!skip_entry) {
			done++;
			ProgressDialog::get_singleton()->task_step("fm_extract", nm, total > 0 ? (done * 100 / total) : 0, true);
		}
	}
	ProgressDialog::get_singleton()->end_task("fm_extract");
	if (!tmp_path.is_empty()) {
		DirAccess::remove_absolute(tmp_path);
	}
	status_label->set_text("Diekstrak ke: " + dest);
	EditorToaster::get_singleton()->popup_str("File manager: ekstrak selesai.");
	_scan_if_inside_project(dest);
	_refresh();
}

void McpFileManager::_do_extract_zip(const String &p_zip) {
	Ref<FileAccess> io_fa;
	zlib_filefunc_def io = zipio_create_io(&io_fa);
	unzFile pkg = unzOpen2(p_zip.utf8().get_data(), &io);
	if (!pkg) {
		status_label->set_text("Bukan file zip valid.");
		return;
	}
	String dest = p_zip.get_base_dir().rstrip("/") + "/" + p_zip.get_file().get_basename() + "/";
	fm_make_dir_recursive(dest);

	// Hitung total file dulu untuk progress real
	int total_files = 0;
	unzFile pkg_count = unzOpen2(p_zip.utf8().get_data(), &io);
	if (pkg_count) {
		int ret = unzGoToFirstFile(pkg_count);
		while (ret == UNZ_OK) {
			unz_file_info info;
			char fname[4096];
			unzGetCurrentFileInfo(pkg_count, &info, fname, 4096, nullptr, 0, nullptr, 0);
			String rel = String::utf8(fname);
			if (!rel.is_absolute_path() && rel.find("..") < 0 && !rel.ends_with("/")) {
				total_files++;
			}
			ret = unzGoToNextFile(pkg_count);
		}
		unzClose(pkg_count);
	}

	ProgressDialog::get_singleton()->add_task("fm_extract", "Mengekstrak zip", total_files);
	int done = 0;
	int ret = unzGoToFirstFile(pkg);
	while (ret == UNZ_OK) {
		unz_file_info info;
		char fname[4096];
		unzGetCurrentFileInfo(pkg, &info, fname, 4096, nullptr, 0, nullptr, 0);
		String rel = String::utf8(fname);
		// Tolak zip-slip: path absolut atau keluar folder tujuan.
		if (!rel.is_absolute_path() && rel.find("..") < 0 && !rel.ends_with("/")) {
			if (unzOpenCurrentFile(pkg) == UNZ_OK) {
				// Streaming write: chunk 64KB untuk anti-OOM
				const int CHUNK_SIZE = 65536;
				Vector<uint8_t> chunk;
				chunk.resize(CHUNK_SIZE);
				String out = dest + rel;
				fm_make_dir_recursive(out.get_base_dir());
				Ref<FileAccess> f = FileAccess::open(out, FileAccess::WRITE);
				if (f.is_valid()) {
					int remaining = info.uncompressed_size;
					while (remaining > 0) {
						int to_read = MIN(CHUNK_SIZE, remaining);
						int read = unzReadCurrentFile(pkg, chunk.ptrw(), to_read);
						if (read <= 0) break;
						f->store_buffer(chunk.ptr(), read);
						remaining -= read;
					}
				}
				unzCloseCurrentFile(pkg);
			}
		}
		done++;
		ProgressDialog::get_singleton()->task_step("fm_extract", rel, (total_files > 0) ? (done * 100 / total_files) : 0, true);
		ret = unzGoToNextFile(pkg);
	}
	unzClose(pkg);
	ProgressDialog::get_singleton()->end_task("fm_extract");
	status_label->set_text("Diekstrak ke: " + dest);
	EditorToaster::get_singleton()->popup_str("File manager: ekstrak selesai.");
	_scan_if_inside_project(dest);
	_refresh();
}

void McpFileManager::_do_import(const Vector<String> &p_src, const String &p_dst_dir) {
	String dest = ProjectSettings::get_singleton()->globalize_path(p_dst_dir);
	if (dest.is_empty()) {
		status_label->set_text("Folder tujuan tidak valid.");
		return;
	}
	fm_make_dir_recursive(dest);
	int count = 0;
	for (const String &s : p_src) {
		if (this->_copy_recursive(s, dest.rstrip("/") + "/" + s.get_file(), count) != OK) {
			status_label->set_text("Gagal import: " + s);
			return;
		}
	}
	EditorFileSystem::get_singleton()->scan_changes();
	status_label->set_text(vformat("Diimport %d item ke %s", count, p_dst_dir));
	EditorToaster::get_singleton()->popup_str("File manager: import selesai, filesystem dipindai.");
}

void McpFileManager::_show_info(const Vector<String> &p_src) {
	if (p_src.is_empty()) {
		status_label->set_text("Pilih minimal 1 item.");
		return;
	}
	String txt;
	for (const String &s : p_src) {
		if (DirAccess::exists(s)) {
			txt += vformat("Folder: %s\n", s);
		} else {
			Ref<FileAccess> f = FileAccess::open(s, FileAccess::READ);
			uint64_t sz = f.is_valid() ? f->get_length() : 0;
			String dt = Time::get_singleton()->get_datetime_string_from_unix_time(int64_t(FileAccess::get_modified_time(s)));
			txt += vformat("File: %s\nUkuran: %s\nDiubah: %s\n\n", s, _fmt_size(sz), dt);
		}
	}
	AcceptDialog *dlg = memnew(AcceptDialog);
	dlg->set_title("Properti");
	Label *lb = memnew(Label);
	lb->set_text(txt);
	lb->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	lb->set_custom_minimum_size(Vector2(420, 200));
	dlg->add_child(lb);
	add_child(dlg);
	dlg->popup_centered();
}

void McpFileManager::_bind_methods() {
}

McpFileManagerPlugin *McpFileManagerPlugin::singleton = nullptr;

McpFileManagerPlugin::McpFileManagerPlugin() {
	singleton = this;
}

McpFileManagerPlugin::~McpFileManagerPlugin() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void McpFileManagerPlugin::_enter_tree() {
	add_tool_menu_item("File Manager", callable_mp(this, &McpFileManagerPlugin::_open_manager));
	// Tombol 1-klik di toolbar atas (lebih gampang dari menu Project > Tools).
	toolbar_btn = memnew(Button);
	toolbar_btn->set_tooltip_text("File Manager (Ctrl+Shift+F)");
	toolbar_btn->set_flat(true);
	toolbar_btn->set_text("FM");
	toolbar_btn->connect("pressed", callable_mp(this, &McpFileManagerPlugin::_open_manager));
	Control *base = EditorInterface::get_singleton()->get_base_control();
	if (base && base->has_theme_icon("fm_dir", "EditorIcons")) {
		toolbar_btn->set_button_icon(base->get_theme_icon("fm_dir", "EditorIcons"));
	}
	// Kunci warna icon ke putih di semua state: cegah tint merah/warna lain
	// dari theme induk (terbukti terjadi di header Inspector).
	toolbar_btn->add_theme_color_override("icon_normal_color", Color(1, 1, 1));
	toolbar_btn->add_theme_color_override("icon_hover_color", Color(1, 1, 1));
	toolbar_btn->add_theme_color_override("icon_pressed_color", Color(1, 1, 1));
	toolbar_btn->add_theme_color_override("icon_focus_color", Color(1, 1, 1));
	toolbar_btn->add_theme_color_override("icon_disabled_color", Color(1, 1, 1, 0.5));
	toolbar_btn->add_theme_color_override("font_color", Color(1, 1, 1));
	add_control_to_container(CONTAINER_TOOLBAR, toolbar_btn);
	// Shortcut default Ctrl+Shift+F, tercatat di EditorSettings agar bisa diubah user.
	EditorSettings *es = EditorSettings::get_singleton();
	if (es && !es->has_setting("shortcuts/mcp_file_manager")) {
		Ref<InputEventKey> k;
		k.instantiate();
		k->set_pressed(true);
		k->set_keycode(Key::F);
		k->set_ctrl_pressed(true);
		k->set_shift_pressed(true);
		es->set_initial_value("shortcuts/mcp_file_manager", k, false);
	}
	set_process_unhandled_key_input(true);
}

void McpFileManagerPlugin::_exit_tree() {
	set_process_unhandled_key_input(false);
	remove_tool_menu_item("File Manager");
	if (toolbar_btn) {
		remove_control_from_container(CONTAINER_TOOLBAR, toolbar_btn);
		toolbar_btn = nullptr;
	}
	if (win) {
		win->hide();
	}
}

void McpFileManagerPlugin::_unhandled_key_input(const Ref<InputEvent> &p_event) {
	EditorSettings *es = EditorSettings::get_singleton();
	if (es == nullptr || p_event.is_null()) {
		return;
	}
	if (!es->has_setting("shortcuts/mcp_file_manager")) {
		return;
	}
	Ref<InputEventKey> want = es->get_setting("shortcuts/mcp_file_manager");
	Ref<InputEventKey> got = p_event;
	if (want.is_null() || got.is_null() || !got->is_pressed() || got->is_echo()) {
		return;
	}
	if (got->get_keycode() == want->get_keycode() &&
			got->is_ctrl_pressed() == want->is_ctrl_pressed() &&
			got->is_shift_pressed() == want->is_shift_pressed() &&
			got->is_alt_pressed() == want->is_alt_pressed()) {
		_open_manager();
		get_viewport()->set_input_as_handled();
	}
}

void McpFileManagerPlugin::_open_manager() {
	print_line("FM: _open_manager dipanggil");
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		print_line("FM: GAGAL - EditorInterface null");
		return;
	}
	if (!win) {
		print_line("FM: buat Window baru");
		win = memnew(Window);
		win->set_title("File Manager");
		win->set_min_size(Vector2i(480, 400));
		fm = memnew(McpFileManager);
		fm->set_anchors_preset(Control::PRESET_FULL_RECT);
		win->add_child(fm);
		ei->get_base_control()->add_child(win);
	}
	if (win->is_visible()) {
		print_line("FM: sembunyikan window");
		win->hide();
	} else {
		print_line("FM: tampilkan window");
		win->popup_centered(Vector2i(680, 480));
		fm->open_dir(fm->get_current_dir().is_empty() ? "/storage/emulated/0/Documents" : fm->get_current_dir());
	}
}

#endif // TOOLS_ENABLED
