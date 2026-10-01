#include "mcp_tools.h"

#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/editor_node.h"
#include "core/config/engine.h"
#include "scene/gui/tree.h"
#include "scene/resources/style_box_flat.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "core/input/input.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "editor/editor_undo_redo_manager.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/ustring.h"
#include "core/io/json.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/editor_interface.h"
#include "editor/settings/editor_settings.h"
#include "mcp_server.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "core/input/input_event.h"
#include "core/object/class_db.h"

#include <mutex>
#include <vector>

// ----------------------------------------------------------------- Log ring
// Small in-process ring buffer backing the `logs_read` tool. Fed by engine
// errors/warnings (via ErrorHandlerList) and by the MCP server lifecycle.
// Kept outside the TOOLS_ENABLED guard because mcp_server.cpp always
// references mcp_log_append().

struct McpLogLine {
	uint64_t msec = 0;
	String text;
	bool is_error = false;
	bool is_warning = false;
};

static std::vector<McpLogLine> mcp_log_ring;
static std::mutex mcp_log_mu;
static const int MCP_LOG_RING_CAP = 500;

void mcp_log_append(const String &p_text, bool p_error, bool p_warning) {
	std::lock_guard<std::mutex> lk(mcp_log_mu);
	mcp_log_ring.push_back(McpLogLine{ Time::get_singleton()->get_ticks_msec(), p_text, p_error, p_warning });
	int overflow = (int)mcp_log_ring.size() - MCP_LOG_RING_CAP;
	if (overflow > 0) {
		mcp_log_ring.erase(mcp_log_ring.begin(), mcp_log_ring.begin() + overflow);
	}
}

#if defined(TOOLS_ENABLED)

Variant mcp_tool_ret_text(const String &p_text) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", p_text } } } }, { "isError", false } };
}

Variant mcp_tool_ret_error(const String &p_text) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", p_text } } } }, { "isError", true } };
}

Variant mcp_tool_ret_json(const Variant &p_value) {
	return Dictionary{ { "content", Array{ Dictionary{ { "type", "text" }, { "text", JSON::stringify(p_value) } } } }, { "isError", false } };
}

// Ask the editor to rescan/reload right after MCP changed files on disk, so
// new/modified scenes, scripts and assets show up immediately (no game run
// or app focus event needed, which never happen on the Android editor).
static void _mcp_refresh_editor() {
	EditorNode *en = EditorNode::get_singleton();
	if (en) {
		en->refresh_external_changes();
	}
}

// ----------------------------------------------------------------- Error hook
// Installed by mcp_register_tools() (TOOLS builds only).

static ErrorHandlerList s_mcp_err_handler;

static void _mcp_log_err_cb(void *p_ud, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_verbose_error, bool p_editor_notify, ErrorHandlerType p_type) {
	String text = vformat("[%s] %s (%s:%d)", p_type == ERR_HANDLER_WARNING ? "PERINGATAN" : "KESALAHAN", p_error, p_file, p_line);
	if (p_verbose_error && *p_verbose_error) {
		text += "\n" + String(p_verbose_error);
	}
	mcp_log_append(text, p_type != ERR_HANDLER_WARNING, p_type == ERR_HANDLER_WARNING);
}

// ----------------------------------------------------------------- Helpers

static Node *_scene_root() {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return nullptr;
	}
	return ei->get_edited_scene_root();
}

static Node *_resolve_node(const String &p_path) {
	Node *root = _scene_root();
	if (!root) {
		return nullptr;
	}
	if (p_path.is_empty() || p_path == "." || p_path == "/") {
		return root;
	}
	String p = p_path.strip_edges();
	while (p.ends_with("/")) {
		p = p.substr(0, p.length() - 1);
	}
	if (p.is_empty()) {
		return root;
	}
	// 1) Scene-relative path, e.g. "MenuPanel/MenuBox" or "MainMenu".
	Node *n = root->get_node_or_null(NodePath(p));
	if (n) {
		return n;
	}
	if (p == root->get_name()) {
		return root;
	}
	// 2) First segment equals the scene root name or scene file base name
	// (clients often send "MainMenu/MenuPanel" or "/root/MainMenu/MenuPanel").
	String root_name = root->get_name();
	String root_file = root->get_scene_file_path().get_file().get_basename();
	Vector<String> segs = p.split("/");
	if (segs.size() >= 2 && (segs[0] == root_name || segs[0] == root_file)) {
		n = root->get_node_or_null(NodePath(p.substr(segs[0].length() + 1)));
		if (n) {
			return n;
		}
	}
	// 3) Tree-absolute path, e.g. "/root/@EditorNode@.../@SubViewport@.../MainMenu/MenuPanel".
	if (p.begins_with("/") && root->get_tree()) {
		n = root->get_tree()->get_root()->get_node_or_null(NodePath(p));
		if (n) {
			Node *check = n->get_parent();
			while (check) {
				if (check == root) {
					return n;
				}
				check = check->get_parent();
			}
		}
	}
	return nullptr;
}

static bool _has_project() {
	return ProjectSettings::get_singleton()->has_setting("application/config/name");
}

static String _scene_rel_path(Node *p_node) {
	Node *root = _scene_root();
	if (!root || !p_node) {
		return String();
	}
	return "/" + String(root->get_path_to(p_node));
}

static bool _wildcard_match(const String &p_pattern, const String &p_str) {
	if (!p_pattern.contains("*")) {
		return p_pattern == p_str;
	}
	Vector<String> parts = p_pattern.split("*");
	int idx = 0;
	for (int i = 0; i < parts.size(); i++) {
		String part = parts[i];
		if (part.is_empty()) {
			continue;
		}
		int found = p_str.find(part, idx);
		if (found == -1) {
			return false;
		}
		if (i == 0 && !p_str.begins_with(part)) {
			return false;
		}
		idx = found + part.length();
	}
	if (!p_pattern.ends_with("*") && !p_str.ends_with(parts[parts.size() - 1])) {
		return false;
	}
	return true;
}

static void _walk_assets(const String &p_dir, Array &r_out, const String &p_pattern, bool p_recursive, int p_depth) {
	if (p_depth > 32) {
		return;
	}
	Ref<DirAccess> d = DirAccess::open(p_dir);
	if (d.is_null()) {
		return;
	}
	d->list_dir_begin();
	String f = d->get_next();
	while (!f.is_empty()) {
		if (f == "." || f == "..") {
			f = d->get_next();
			continue;
		}
		String full = p_dir.path_join(f);
		if (d->current_is_dir()) {
			if (f == ".godot") {
				f = d->get_next();
				continue;
			}
			if (p_recursive) {
				_walk_assets(full, r_out, p_pattern, true, p_depth + 1);
			}
		} else {
			if (_wildcard_match(p_pattern, full.replace("res://", ""))) {
				Dictionary entry;
				entry["path"] = full.replace("res://", "");
				entry["size"] = FileAccess::get_size(full);
				r_out.append(entry);
			}
		}
		f = d->get_next();
	}
	d->list_dir_end();
	d.unref();
}

static void _walk_scene(Node *p_node, Node *p_root, Dictionary &r_out) {
	r_out["name"] = p_node->get_name();
	r_out["type"] = p_node->get_class();
	r_out["path"] = p_root->get_path_to(p_node);
	if (p_node->get_script_instance()) {
		Ref<Script> scr = p_node->get_script();
		if (scr.is_valid()) {
			r_out["script"] = scr->get_path();
		}
	}
	Dictionary props;
	if (p_node->has_method("get")) {
		bool ok = false;
		Variant v = p_node->get("position", &ok);
		if (ok && (v.get_type() == Variant::VECTOR2 || v.get_type() == Variant::VECTOR3)) {
			props["position"] = v;
		}
		v = p_node->get("rotation", &ok);
		if (ok) {
			props["rotation"] = v;
		}
		v = p_node->get("scale", &ok);
		if (ok && (v.get_type() == Variant::VECTOR2 || v.get_type() == Variant::VECTOR3)) {
			props["scale"] = v;
		}
		v = p_node->get("visible", &ok);
		if (ok && v.get_type() == Variant::BOOL) {
			props["visible"] = v;
		}
		v = p_node->get("text", &ok);
		if (ok && (v.get_type() == Variant::STRING || v.get_type() == Variant::STRING_NAME)) {
			props["text"] = v;
		}
	}
	if (!props.is_empty()) {
		r_out["properties"] = props;
	}
	Array children;
	int n = p_node->get_child_count();
	for (int i = 0; i < n; i++) {
		Node *c = p_node->get_child(i);
		if (c->get_owner() != p_root && c != p_root) {
			continue;
		}
		if (String(c->get_name()).to_lower() == "editorpaint" && c->get_class() == "SubViewport") {
			continue;
		}
		Dictionary child;
		_walk_scene(c, p_root, child);
		children.append(child);
	}
	if (!children.is_empty()) {
		r_out["children"] = children;
	}
}

static void _append_icon(Dictionary &r_ret, const Ref<Image> &p_image) {
	PackedByteArray png = p_image->save_png_to_buffer();
	String b64 = CryptoCore::b64_encode_str(png.ptr(), png.size());
	Dictionary img;
	img["type"] = "image";
	img["data"] = b64;
	img["mimeType"] = "image/png";
	r_ret["content"] = Array{ img };
	r_ret["isError"] = false;
}

// ----------------------------------------------------------------- Tools

static Variant _tool_project_info(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("Tidak ada proyek yang sedang terbuka.");
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	Dictionary info;
	info["name"] = ps->get_setting("application/config/name", String());
	info["main_scene"] = ps->get_setting("application/run/main_scene", String());
	info["path"] = ProjectSettings::get_singleton()->globalize_path("res://");
	info["version"] = String("4.8-dev");
	return mcp_tool_ret_json(info);
}

static Variant _tool_list_assets(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("Tidak ada proyek yang sedang terbuka.");
	}
	String pattern = p_args.get("pattern", "*");
	bool recursive = p_args.get("recursive", true);
	Array out;
	_walk_assets("res://", out, pattern, recursive, 0);
	return mcp_tool_ret_json(out);
}

static Variant _tool_read_file(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Argumen 'path' wajib diisi.");
	}
	if (!FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("File tidak ditemukan: %s", path));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat membuka file: %s", path));
	}
	String content = f->get_as_text();
	if (p_args.get("json", false)) {
		Variant parsed = JSON::parse_string(content);
		if (parsed.get_type() != Variant::NIL) {
			return mcp_tool_ret_json(parsed);
		}
	}
	return mcp_tool_ret_text(content);
}

// Extract the uid="uid://..." value from a .tscn [gd_scene] header, or "".
static String _tscn_header_uid(const String &p_content) {
	int h = p_content.find("[gd_scene");
	if (h == -1) {
		return String();
	}
	int end = p_content.find("\n", h);
	if (end == -1) {
		end = p_content.length();
	}
	String header = p_content.substr(h, end - h);
	int p = header.find("uid=\"uid://");
	if (p == -1) {
		return String();
	}
	int q = header.find("\"", p + 5);
	if (q == -1) {
		return String();
	}
	return header.substr(p + 5, q - (p + 5));
}

// Rewrite the [gd_scene] header so its uid matches p_uid, inserting the
// attribute when missing and replacing it when it differs.
static String _tscn_set_header_uid(const String &p_content, const String &p_uid) {
	int h = p_content.find("[gd_scene");
	if (h == -1) {
		return p_content;
	}
	int end = p_content.find("\n", h);
	if (end == -1) {
		end = p_content.length();
	}
	String header = p_content.substr(h, end - h);
	String new_header;
	if (header.contains("uid=")) {
		int p = header.find("uid=");
		int q = header.find("\"", p + 4);
		if (q == -1) {
			return p_content;
		}
		new_header = header.substr(0, p) + "uid=\"" + p_uid + "\"" + header.substr(q + 1);
	} else {
		new_header = header + " uid=\"" + p_uid + "\"";
	}
	return p_content.substr(0, h) + new_header + p_content.substr(end);
}

static Variant _tool_write_file(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String content = p_args.get("content", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Argumen 'path' wajib diisi.");
	}
	bool uid_preserved = false;
	if (path.ends_with(".tscn") && FileAccess::exists(path)) {
		Ref<FileAccess> of = FileAccess::open(path, FileAccess::READ);
		if (of.is_valid()) {
			String old_uid = _tscn_header_uid(of->get_as_text());
			if (!old_uid.is_empty() && _tscn_header_uid(content) != old_uid) {
				content = _tscn_set_header_uid(content, old_uid);
				uid_preserved = true;
			}
		}
	}
	// Ensure parent dirs exist.
	String dir = path.get_base_dir();
	if (!dir.is_empty() && dir != "." && !dir.begins_with("res://")) {
		DirAccess::make_dir_recursive_absolute(dir);
	} else if (dir.begins_with("res://")) {
		DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(dir));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat menulis file: %s", path));
	}
	f->store_string(content);
	f->close();
	_mcp_refresh_editor();
	if (uid_preserved) {
		return mcp_tool_ret_text(vformat("Berhasil menulis %d byte ke %s (UID scene asli dipertahankan)", content.utf8().length(), path));
	}
	return mcp_tool_ret_text(vformat("Berhasil menulis %d byte ke %s", content.utf8().length(), path));
}

static Variant _tool_get_project_setting(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("Tidak ada proyek yang sedang terbuka.");
	}
	String name = p_args.get("name", String());
	if (name.is_empty()) {
		return mcp_tool_ret_error("Argumen 'name' wajib diisi.");
	}
	Variant value = ProjectSettings::get_singleton()->get_setting(name);
	if (value.get_type() == Variant::NIL) {
		return mcp_tool_ret_error(vformat("Setting tidak ditemukan: %s", name));
	}
	Dictionary out;
	out["name"] = name;
	out["value"] = value;
	return mcp_tool_ret_json(out);
}

static Variant _tool_set_project_setting(const Dictionary &p_args) {
	if (!_has_project()) {
		return mcp_tool_ret_error("Tidak ada proyek yang sedang terbuka.");
	}
	String name = p_args.get("name", String());
	if (name.is_empty()) {
		return mcp_tool_ret_error("Argumen 'name' wajib diisi.");
	}
	Variant value = p_args.get("value", Variant());
	ProjectSettings::get_singleton()->set_setting(name, value);
	if (p_args.get("save", true)) {
		ProjectSettings::get_singleton()->save();
	}
	return mcp_tool_ret_text(vformat("Setting \'%s\' diperbarui.", name));
}

static Variant _tool_get_scene_tree(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	Node *root = _scene_root();
	if (!root) {
		return mcp_tool_ret_error("Tidak ada scene yang terbuka.");
	}
	Dictionary tree;
	tree["scene_path"] = root->get_scene_file_path().is_empty() ? String("<untitled>") : root->get_scene_file_path();
	Dictionary r;
	_walk_scene(root, root, r);
	tree["root"] = r;
	return mcp_tool_ret_json(tree);
}

static Variant _tool_open_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	String path = p_args.get("path", String());
	if (path.is_empty() || !path.ends_with(".tscn") || !FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene tidak ditemukan: %s", path));
	}
	// Buka scene.
	ei->open_scene_from_path(path);
	// Validasi: cek apakah scene benar-benar terbuka (root valid).
	Node *root = ei->get_edited_scene_root();
	if (!root) {
		return mcp_tool_ret_error(vformat("Scene gagal dibuka (invalid/corrupt): %s", path));
	}
	return mcp_tool_ret_text(vformat("Scene dibuka: %s", path));
}

static Variant _tool_save_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	String path = p_args.get("path", String());
	if (!path.is_empty()) {
		if (path.ends_with(".tscn") || path.ends_with(".scn")) {
			ei->save_scene_as(path);
		} else {
			return mcp_tool_ret_error("Path harus diakhiri .tscn atau .scn");
		}
	} else {
		ei->save_scene();
	}
	return mcp_tool_ret_text("Scene berhasil disimpan.");
}

static Variant _tool_create_scene(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String root_class = p_args.get("root_class", "Node");
	String root_name = p_args.get("root_name", "Root");
	if (path.is_empty() || !path.ends_with(".tscn")) {
		return mcp_tool_ret_error("Missing 'path' (must end with .tscn).");
	}
	if (FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene sudah ada: %s", path));
	}
	String content = vformat("[gd_scene format=3]\n\n[node name=\"%s\" type=\"%s\"]\n", root_name, root_class);
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat menulis scene: %s", path));
	}
	f->store_string(content);
	f->close();
	EditorInterface *ei = EditorInterface::get_singleton();
	if (ei) {
		ei->open_scene_from_path(path);
	}
	_mcp_refresh_editor();
	return mcp_tool_ret_text(vformat("Scene dibuat: %s", path));
}

static Variant _tool_add_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("Tidak ada scene yang terbuka.");
	}
	String class_name = p_args.get("class", "Node");
	String name = p_args.get("name", String());
	String parent_path = p_args.get("parent", String());
	Node *parent = parent_path.is_empty() || parent_path == "." ? root : _resolve_node(parent_path);
	if (!parent) {
		return mcp_tool_ret_error(vformat("Parent tidak ditemukan: %s", parent_path));
	}
	if (!ClassDB::class_exists(class_name)) {
		return mcp_tool_ret_error(vformat("Class tidak ditemukan: %s", class_name));
	}
	Object *obj = ClassDB::instantiate(class_name);
	Node *node = Object::cast_to<Node>(obj);
	if (!node) {
		memdelete(obj);
		return mcp_tool_ret_error(vformat("Class bukan Node: %s", class_name));
	}
	String final_name = name.is_empty() ? class_name : name;
	node->set_name(final_name);
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: menambahkan node %s", final_name));
	ur->add_do_method(parent, "add_child", node, true);
	ur->add_do_method(node, "set_owner", root);
	ur->add_undo_method(node, "set_owner", (Object *)nullptr);
	ur->add_undo_method(parent, "remove_child", node);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Menambahkan %s \'%s\' di bawah %s", class_name, final_name, _scene_rel_path(parent)));
}

static Variant _tool_remove_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("Tidak ada scene yang terbuka.");
	}
	String path = p_args.get("path", String());
	Node *node = _resolve_node(path);
	if (!node || node == root) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", path));
	}
	Node *parent = node->get_parent();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: menghapus node %s", node->get_name()));
	ur->add_do_method(parent, "remove_child", node);
	ur->add_undo_method(parent, "add_child", node, true);
	ur->add_undo_method(node, "set_owner", root);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Node dihapus: %s", path));
}

static Variant _tool_rename_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	String path = p_args.get("path", String());
	String new_name = p_args.get("name", String());
	Node *node = _resolve_node(path);
	if (!node) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", path));
	}
	String old_name = node->get_name();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: mengganti nama node %s", old_name));
	ur->add_do_method(node, "set_name", new_name);
	ur->add_undo_method(node, "set_name", old_name);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Diganti nama %s -> %s", path, new_name));
}

// Resolve a property name that may be a dynamic (runtime generated) property,
// e.g. "theme_override_styles" on Control, which is not in ClassDB but appears
// in get_property_list() as "theme_override_styles/<item>". Returns true when
// found; r_name receives the concrete property to read/write.
static bool _resolve_property(Node *p_node, const String &p_prop, String &r_name) {
	bool ok = false;
	p_node->get(p_prop, &ok);
	if (ok) {
		r_name = p_prop;
		return true;
	}
	List<PropertyInfo> pinfo;
	p_node->get_property_list(&pinfo);
	for (const PropertyInfo &E : pinfo) {
		if (E.name == p_prop) {
			r_name = p_prop;
			return true;
		}
		if (E.name.begins_with(p_prop + "/")) {
			r_name = E.name;
			return true;
		}
	}
	return false;
}

static Variant _tool_get_node_property(const Dictionary &p_args) {
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!node) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", p_args.get("path", String())));
	}
	String prop = p_args.get("property", String());
	if (prop.is_empty()) {
		return mcp_tool_ret_error("Argumen 'property' wajib diisi.");
	}
	String concrete;
	if (!_resolve_property(node, prop, concrete)) {
		return mcp_tool_ret_error(vformat("Property tidak ditemukan: %s", prop));
	}
	bool ok = false;
	Variant v = node->get(concrete, &ok);
	if (!ok) {
		return mcp_tool_ret_error(vformat("Property tidak dapat dibaca: %s", concrete));
	}
	Dictionary out;
	out["path"] = p_args.get("path", String());
	out["property"] = concrete;
	out["value"] = v;
	return mcp_tool_ret_json(out);
}

static Variant _tool_set_node_property(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!ei || !node) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", p_args.get("path", String())));
	}
	String prop = p_args.get("property", String());
	if (prop.is_empty()) {
		return mcp_tool_ret_error("Argumen 'property' wajib diisi.");
	}
	String concrete;
	if (!_resolve_property(node, prop, concrete)) {
		return mcp_tool_ret_error(vformat("Property tidak ditemukan: %s", prop));
	}
	Variant value = p_args.get("value", Variant());

	// Auto-convert {r,g,b,a} JSON to Color(r,g,b,a) for color properties
	if (prop.begins_with("color") || prop.begins_with("theme_override_colors")) {
		if (value.get_type() == Variant::DICTIONARY) {
			Dictionary d = value;
			float r = d.get("r", 0.0);
			float g = d.get("g", 0.0);
			float b = d.get("b", 0.0);
			float a = d.get("a", 1.0);
			if (r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1 && a >= 0 && a <= 1) {
				value = Color(r, g, b, a);
			}
		}
	}

	// Auto-construct StyleBoxFlat resource from inline dict for theme_override_styles
	if (prop.begins_with("theme_override_styles")) {
		if (value.get_type() == Variant::DICTIONARY) {
			Dictionary d = value;
			if (d.has("type") && d["type"] == "StyleBoxFlat") {
				Ref<StyleBoxFlat> sb;
				sb.instantiate();
				if (d.has("bg_color")) {
					Variant bg = d["bg_color"];
					if (bg.get_type() == Variant::STRING) {
						sb->set_bg_color(Color(bg));
					} else if (bg.get_type() == Variant::COLOR) {
						sb->set_bg_color(bg);
					}
				}
						if (d.has("border_width")) {
			sb->set_border_width_all(d["border_width"]);
		}
		if (d.has("border_width_left")) {
			sb->set_border_width(SIDE_LEFT, d["border_width_left"]);
		}
		if (d.has("border_width_right")) {
			sb->set_border_width(SIDE_RIGHT, d["border_width_right"]);
		}
		if (d.has("border_width_top")) {
			sb->set_border_width(SIDE_TOP, d["border_width_top"]);
		}
		if (d.has("border_width_bottom")) {
			sb->set_border_width(SIDE_BOTTOM, d["border_width_bottom"]);
		}
		// Corner radii
		if (d.has("corner_radius")) {
			sb->set_corner_radius_all(d["corner_radius"]);
		}
		if (d.has("corner_radius_top_left") || d.has("corner_radius_top_right") ||
			d.has("corner_radius_bottom_left") || d.has("corner_radius_bottom_right")) {
			int tl = d.has("corner_radius_top_left") ? (int)d["corner_radius_top_left"] : 0;
			int tr = d.has("corner_radius_top_right") ? (int)d["corner_radius_top_right"] : 0;
			int br = d.has("corner_radius_bottom_right") ? (int)d["corner_radius_bottom_right"] : 0;
			int bl = d.has("corner_radius_bottom_left") ? (int)d["corner_radius_bottom_left"] : 0;
			sb->set_corner_radius_individual(tl, tr, br, bl);
		}
		// Expand margins
		if (d.has("expand_margin")) {
			sb->set_expand_margin_all(d["expand_margin"]);
		}
		if (d.has("expand_margin_left") || d.has("expand_margin_top") ||
			d.has("expand_margin_right") || d.has("expand_margin_bottom")) {
			float l = d.has("expand_margin_left") ? (float)d["expand_margin_left"] : 0.0f;
			float t = d.has("expand_margin_top") ? (float)d["expand_margin_top"] : 0.0f;
			float r = d.has("expand_margin_right") ? (float)d["expand_margin_right"] : 0.0f;
			float b = d.has("expand_margin_bottom") ? (float)d["expand_margin_bottom"] : 0.0f;
			sb->set_expand_margin_individual(l, t, r, b);
		}
	// Shadow
				if (d.has("shadow_color")) {
					Variant sc = d["shadow_color"];
					if (sc.get_type() == Variant::STRING) {
						sb->set_shadow_color(Color(sc));
					} else if (sc.get_type() == Variant::COLOR) {
						sb->set_shadow_color(sc);
					}
				}

				if (d.has("shadow_offset")) {
					Variant so = d["shadow_offset"];
					if (so.get_type() == Variant::DICTIONARY) {
						Dictionary sod = so;
						sb->set_shadow_offset(Vector2(sod.get("x", 0.0), sod.get("y", 0.0)));
					}
				}
				if (d.has("shadow_size")) {
					sb->set_shadow_size(d["shadow_size"]);
				}
				// Draw center
				if (d.has("draw_center")) {
					sb->set_draw_center(d["draw_center"]);
				}
				value = sb;
			}
		}
	}

	// Capture old value for undo.
	bool ok = false;
	Variant old = node->get(concrete, &ok);

	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: menetapkan %s.%s", p_args.get("path", String()), concrete));
	ur->add_do_method(node, "set", concrete, value);
	ur->add_undo_method(node, "set", concrete, old);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Menetapkan %s.%s", p_args.get("path", String()), concrete));
}

static Variant _tool_reparent_node(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *root = _scene_root();
	if (!ei || !root) {
		return mcp_tool_ret_error("Tidak ada scene yang terbuka.");
	}
	Node *node = _resolve_node(p_args.get("path", String()));
	Node *new_parent = _resolve_node(p_args.get("new_parent", String()));
	if (!node || node == root) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", p_args.get("path", String())));
	}
	if (!new_parent) {
		return mcp_tool_ret_error(vformat("Parent tidak ditemukan: %s", p_args.get("new_parent", String())));
	}
	Node *old_parent = node->get_parent();
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: memindahkan %s", node->get_name()));
	ur->add_do_method(new_parent, "add_child", node, true);
	ur->add_do_method(node, "set_owner", root);
	ur->add_undo_method(old_parent, "add_child", node, true);
	ur->add_undo_method(node, "set_owner", root);
	ur->commit_action();
	return mcp_tool_ret_text(vformat("Memindahkan %s di bawah %s", node->get_name(), _scene_rel_path(new_parent)));
}

static Variant _tool_read_script(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Argumen 'path' wajib diisi.");
	}
	if (!FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Script tidak ditemukan: %s", path));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat membuka script: %s", path));
	}
	return mcp_tool_ret_text(f->get_as_text());
}

static Variant _tool_write_script(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String content = p_args.get("content", String());
	if (path.is_empty() || !path.ends_with(".gd")) {
		return mcp_tool_ret_error("'path' harus diakhiri .gd");
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat menulis script: %s", path));
	}
	f->store_string(content);
	f->close();
	_mcp_refresh_editor();
	return mcp_tool_ret_text(vformat("Script ditulis: %s (%d byte)", path, content.utf8().length()));
}

static Variant _tool_attach_script(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	Node *node = _resolve_node(p_args.get("path", String()));
	if (!ei || !node) {
		return mcp_tool_ret_error(vformat("Node tidak ditemukan: %s", p_args.get("path", String())));
	}
	String script_path = p_args.get("script", String());
	if (script_path.is_empty()) {
		return mcp_tool_ret_error("Argumen 'script' (path) wajib diisi.");
	}
	if (!FileAccess::exists(script_path)) {
		Ref<FileAccess> f = FileAccess::open(script_path, FileAccess::WRITE);
		if (f.is_null()) {
			return mcp_tool_ret_error(vformat("Tidak dapat membuat script: %s", script_path));
		}
		f->store_string("extends Node\n");
		f->close();
	}
	Ref<Script> script = ResourceLoader::load(script_path);
	if (script.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat memuat script: %s", script_path));
	}
	EditorUndoRedoManager *ur = ei->get_editor_undo_redo();
	ur->create_action(vformat("MCP: melampirkan %s ke %s", script_path, node->get_path()));
	ur->add_do_method(node, "set_script", script);
	ur->add_undo_method(node, "set_script", (Object *)nullptr);
	ur->commit_action();
	_mcp_refresh_editor();
	return mcp_tool_ret_text(vformat("Script %s dilampirkan ke %s", script_path, node->get_path()));
}

static Variant _tool_run_main_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	if (ei->is_playing_scene()) {
		return mcp_tool_ret_error("Game sudah berjalan.");
	}
	ei->play_main_scene();
	return mcp_tool_ret_text("Memulai main scene.");
}

static Variant _tool_run_custom_scene(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	String path = p_args.get("path", String());
	if (path.is_empty() || !FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Scene tidak ditemukan: %s", path));
	}
	if (ei->is_playing_scene()) {
		return mcp_tool_ret_error("Game sudah berjalan.");
	}
	ei->play_custom_scene(path);
	return mcp_tool_ret_text(vformat("Memulai scene %s", path));
}

static Variant _tool_stop_game(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	if (!ei->is_playing_scene()) {
		return mcp_tool_ret_text("Game tidak sedang berjalan.");
	}
	ei->stop_playing_scene();
	return mcp_tool_ret_text("Game dihentikan.");
}

static Variant _tool_game_state(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	Dictionary st;
	st["playing"] = ei->is_playing_scene();
	Node *root = _scene_root();
	if (root) {
		st["current_scene"] = root->get_scene_file_path();
	}
	return mcp_tool_ret_json(st);
}

static Key _key_from_name(const String &p_name) {
	if (p_name.is_empty()) {
		return Key::NONE;
	}
	if (p_name.is_valid_int()) {
		return (Key)p_name.to_int();
	}
	String n = p_name.to_upper();
	if (n.length() == 1 && n[0] >= 'A' && n[0] <= 'Z') {
		return (Key)(Key::A + (n[0] - 'A'));
	}
	if (n.length() == 1 && n[0] >= '0' && n[0] <= '9') {
		return (Key)(Key::KEY_0 + (n[0] - '0'));
	}
	struct KeyName {
		const char *name;
		Key key;
	};
	static const KeyName names[] = {
		{ "SPACE", Key::SPACE }, { "ENTER", Key::ENTER }, { "RETURN", Key::ENTER }, { "ESCAPE", Key::ESCAPE },
		{ "TAB", Key::TAB }, { "SHIFT", Key::SHIFT }, { "CTRL", Key::CTRL }, { "CONTROL", Key::CTRL },
		{ "ALT", Key::ALT }, { "LEFT", Key::LEFT }, { "RIGHT", Key::RIGHT }, { "UP", Key::UP }, { "DOWN", Key::DOWN },
		{ "BACKSPACE", Key::BACKSPACE }, { "DELETE", Key::KEY_DELETE }, { "HOME", Key::HOME }, { "END", Key::END },
		{ "PAGEUP", Key::PAGEUP }, { "PAGEDOWN", Key::PAGEDOWN }, { "F1", Key::F1 }, { "F2", Key::F2 },
		{ "F3", Key::F3 }, { "F4", Key::F4 }, { "F5", Key::F5 }, { "F6", Key::F6 }, { "F7", Key::F7 },
		{ "F8", Key::F8 }, { "F9", Key::F9 }, { "F10", Key::F10 }, { "F11", Key::F11 }, { "F12", Key::F12 },
	};
	for (const KeyName &kn : names) {
		if (n == kn.name) {
			return kn.key;
		}
	}
	return Key::NONE;
}

static Variant _tool_send_input(const Dictionary &p_args) {
	Input *in = Input::get_singleton();
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!in || !ei) {
		return mcp_tool_ret_error("Input tidak tersedia.");
	}
	String kind = p_args.get("kind", "action");
	if (kind == "action") {
		String action = p_args.get("action", String());
		if (action.is_empty()) {
			return mcp_tool_ret_error("Argumen 'action' wajib diisi.");
		}
		if (p_args.get("pressed", true)) {
			in->action_press(action);
		} else {
			in->action_release(action);
		}
		return mcp_tool_ret_text(vformat("Aksi %s %s", action, p_args.get("pressed", true) ? "ditekan" : "dilepas"));
	}
	if (kind == "key") {
		Key keycode = _key_from_name(p_args.get("key", String()));
		if (keycode == Key::NONE) {
			return mcp_tool_ret_error(vformat("Nama key tidak dikenal: %s", p_args.get("key", String())));
		}
		Ref<InputEventKey> ev = memnew(InputEventKey);
		ev->set_keycode(keycode);
		ev->set_physical_keycode(keycode);
		ev->set_pressed(p_args.get("pressed", true));
		in->parse_input_event(ev);
		return mcp_tool_ret_text(vformat("Key %s %s", p_args.get("key", String()), p_args.get("pressed", true) ? "ditekan" : "dilepas"));
	}
	if (kind == "mouse_button") {
		Ref<InputEventMouseButton> ev = memnew(InputEventMouseButton);
		ev->set_button_index((MouseButton)(int)p_args.get("button", 1));
		ev->set_pressed(p_args.get("pressed", true));
		if (p_args.has("position")) {
			Array pos = p_args["position"];
			if (pos.size() >= 2) {
				ev->set_position(Vector2(pos[0], pos[1]));
				ev->set_global_position(Vector2(pos[0], pos[1]));
			}
		}
		in->parse_input_event(ev);
		return mcp_tool_ret_text("Tombol mouse dikirim.");
	}
	return mcp_tool_ret_error(vformat("Jenis input tidak dikenal: %s", kind));
}

static Variant _tool_screenshot(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei) {
		return mcp_tool_ret_error("Editor tidak tersedia.");
	}
	String source = p_args.get("source", "editor");
	Ref<Image> img;
	if (source == "game" && ei->is_playing_scene()) {
		Window *w = SceneTree::get_singleton()->get_root();
		if (w) {
			img = w->get_texture()->get_image();
		}
	}
	if (img.is_null()) {
		SubViewport *vp = source == "2d" ? ei->get_editor_viewport_2d() : ei->get_editor_viewport_3d();
		if (vp) {
			img = vp->get_texture()->get_image();
		}
	}
	if (img.is_null()) {
		return mcp_tool_ret_error("Tidak dapat mengambil screenshot (apakah viewport tersedia?).");
	}
	Dictionary ret;
	_append_icon(ret, img);
	return ret;
}

// ----------------------------------------------------- godot-ai style tools

static String _tool_op(const Dictionary &p_args, const String &p_default = String()) {
	return p_args.get("op", p_default);
}

static String _unknown_op(const String &p_op, const String &p_valid) {
	return vformat("Operasi '%s' tidak dikenal. Operasi valid: %s", p_op, p_valid);
}

static Variant _tool_editor_state(const Dictionary &p_args) {
	EditorInterface *ei = EditorInterface::get_singleton();
	McpServer *s = McpServer::get_singleton();
	Dictionary st;
	String readiness = "ready";
	if (ei) {
		if (ei->get_resource_filesystem()->is_scanning()) {
			readiness = "importing";
		} else if (ei->is_playing_scene()) {
			readiness = "playing";
		} else if (ei->get_edited_scene_root() == nullptr) {
			readiness = "no_scene";
		}
	}
	st["readiness"] = readiness;
	if (ei) {
		st["playing"] = ei->is_playing_scene();
		Node *root = ei->get_edited_scene_root();
		if (root) {
			st["current_scene"] = root->get_scene_file_path();
		}
	}
	st["server_running"] = s ? s->is_running() : false;
	st["url"] = s ? s->get_mcp_url() : String();
	st["port"] = s ? s->get_port() : -1;
	st["enabled"] = s ? s->get_enabled() : false;
	st["godot_version"] = Engine::get_singleton()->get_version_info().get("string", String());
	return mcp_tool_ret_json(st);
}

static Variant _session_info() {
	Dictionary d;
	d["status"] = "active";
	d["project_path"] = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path("res://") : String();
	d["godot_version"] = Engine::get_singleton()->get_version_info().get("string", String());
	Node *root = _scene_root();
	if (root) {
		d["current_scene"] = root->get_scene_file_path();
	}
	return d;
}

static Variant _tool_session_activate(const Dictionary &p_args) {
	return mcp_tool_ret_json(_session_info());
}

static Variant _tool_session_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "info");
	if (op == "info" || op == "activate") {
		return mcp_tool_ret_json(_session_info());
	}
	return mcp_tool_ret_error(_unknown_op(op, "info|activate"));
}

static Variant _tool_node_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "remove");
	if (op == "remove") {
		return _tool_remove_node(p_args);
	}
	if (op == "rename") {
		return _tool_rename_node(p_args);
	}
	if (op == "reparent") {
		return _tool_reparent_node(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "remove|rename|reparent"));
}

static Variant _tool_scene_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "create");
	if (op == "create") {
		return _tool_create_scene(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "create"));
}

static Variant _tool_filesystem_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "read");
	if (op == "read") {
		return _tool_read_file(p_args);
	}
	if (op == "write") {
		return _tool_write_file(p_args);
	}
	if (op == "list") {
		return _tool_list_assets(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "read|write|list"));
}

static Variant _tool_script_patch(const Dictionary &p_args) {
	String path = p_args.get("path", String());
	String find = p_args.get("find", String());
	String replace = p_args.get("replace", String());
	if (path.is_empty()) {
		return mcp_tool_ret_error("Argumen 'path' wajib diisi.");
	}
	if (find.is_empty()) {
		return mcp_tool_ret_error("Argumen 'find' wajib diisi.");
	}
	if (!FileAccess::exists(path)) {
		return mcp_tool_ret_error(vformat("Script tidak ditemukan: %s", path));
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat membuka script: %s", path));
	}
	String content = f->get_as_text();
	f->close();
	bool all = p_args.get("all", false);
	int count = 0;
	int idx = 0;
	while (true) {
		int pos = content.find(find, idx);
		if (pos == -1) {
			break;
		}
		content = content.substr(0, pos) + replace + content.substr(pos + find.length());
		idx = pos + replace.length();
		count++;
		if (!all) {
			break;
		}
	}
	if (count == 0) {
		return mcp_tool_ret_error(vformat("Pola tidak ditemukan di %s", path));
	}
	Ref<FileAccess> w = FileAccess::open(path, FileAccess::WRITE);
	if (w.is_null()) {
		return mcp_tool_ret_error(vformat("Tidak dapat menulis script: %s", path));
	}
	w->store_string(content);
	w->close();
	_mcp_refresh_editor();
	return mcp_tool_ret_text(vformat("Ditambal %s (%d penggantian)", path, count));
}

static Variant _tool_project_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "info");
	if (op == "info") {
		return _tool_project_info(p_args);
	}
	if (op == "get_setting") {
		return _tool_get_project_setting(p_args);
	}
	if (op == "set_setting") {
		return _tool_set_project_setting(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "info|get_setting|set_setting"));
}

static Variant _tool_project_run(const Dictionary &p_args) {
	String op = _tool_op(p_args, "play");
	if (op == "play" || op == "run") {
		return _tool_run_main_scene(p_args);
	}
	if (op == "run_scene" || op == "run_custom") {
		return _tool_run_custom_scene(p_args);
	}
	if (op == "stop" || op == "quit") {
		return _tool_stop_game(p_args);
	}
	if (op == "state") {
		return _tool_game_state(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "play|run_scene|stop|state"));
}

static Variant _tool_game_manage(const Dictionary &p_args) {
	String op = _tool_op(p_args, "state");
	if (op == "state") {
		return _tool_game_state(p_args);
	}
	if (op == "send_action") {
		Dictionary a = p_args;
		a["kind"] = "action";
		return _tool_send_input(a);
	}
	if (op == "send_key") {
		Dictionary a = p_args;
		a["kind"] = "key";
		return _tool_send_input(a);
	}
	if (op == "send_mouse") {
		Dictionary a = p_args;
		a["kind"] = "mouse_button";
		return _tool_send_input(a);
	}
	if (op == "input") {
		return _tool_send_input(p_args);
	}
	return mcp_tool_ret_error(_unknown_op(op, "state|send_action|send_key|send_mouse|input"));
}

static Variant _tool_batch_execute(const Dictionary &p_args) {
	McpServer *s = McpServer::get_singleton();
	Variant ops_v = p_args.get("operations", Variant());
	Array ops;
	if (ops_v.get_type() == Variant::ARRAY) {
		ops = ops_v;
	} else if (ops_v.get_type() == Variant::STRING) {
		// Parse JSON string if operations sent as JSON string.
		Variant parsed = JSON::parse_string(ops_v);
		if (parsed.get_type() == Variant::ARRAY) {
			ops = parsed;
		}
	}
	if (ops.is_empty() && p_args.has("tools")) {
		ops = p_args.get("tools", Array());
	}
	if (ops.is_empty()) {
		return mcp_tool_ret_error("Missing 'operations' (array of {'tool': name, 'arguments': ...}). Gunakan bentuk: {\"operations\": [{\"tool\": \"...\", \"arguments\": {...}}]} atau kirim langsung array sebagai arguments.");
	}
	bool stop_on_error = p_args.get("stop_on_error", true);
	Array results;
	for (int i = 0; i < ops.size(); i++) {
		Variant v = ops[i];
		if (v.get_type() != Variant::DICTIONARY) {
			results.append(mcp_tool_ret_error(vformat("operations[%d] bukan sebuah objek", i)));
			if (stop_on_error) {
				break;
			}
			continue;
		}
		Dictionary op = v;
		String tool = op.get("tool", String());
		if (tool.is_empty()) {
			tool = op.get("name", String());
		}
		if (tool.is_empty()) {
			results.append(mcp_tool_ret_error(vformat("operations[%d] tidak memiliki \'tool\'", i)));
			if (stop_on_error) {
				break;
			}
			continue;
		}
		Dictionary args = op.get("arguments", Dictionary());
		if (args.is_empty() && op.has("args")) {
			Variant alt = op.get("args", Variant());
			if (alt.get_type() == Variant::DICTIONARY) {
				args = alt;
			}
		}
		Dictionary res = s->execute_tool(tool, args);
		results.append(res);
		if (stop_on_error && bool(res.get("isError", false))) {
			break;
		}
	}
	Dictionary out;
	Dictionary content_item;
	content_item["type"] = "text";
	content_item["text"] = JSON::stringify(results);
	out["content"] = Array{ content_item };
	out["isError"] = false;
	out["results"] = results;
	return out;
}

static Variant _tool_logs_read(const Dictionary &p_args) {
	String level = p_args.get("level", "all");
	int requested = (int)p_args.get("limit", 200);
	int limit = requested > 1 ? requested : 1;
	bool want_error = level == "all" || level == "error";
	bool want_warning = level == "all" || level == "warning";
	bool want_info = level == "all" || level == "info";
	Array lines;
	std::lock_guard<std::mutex> lk(mcp_log_mu);
	int total = (int)mcp_log_ring.size();
	int start = 0;
	if (total > limit) {
		start = total - limit;
	}
	for (int i = start; i < total; i++) {
		const McpLogLine &l = mcp_log_ring[i];
		bool keep = l.is_error ? want_error : (l.is_warning ? want_warning : want_info);
		if (!keep) {
			continue;
		}
		Dictionary d;
		d["level"] = l.is_error ? "error" : (l.is_warning ? "warning" : "info");
		d["text"] = l.text;
		d["msec"] = (double)l.msec;
		lines.append(d);
	}
	return mcp_tool_ret_json(lines);
}

static Variant _tool_debugger_errors(const Dictionary &p_args) {
	// Reads the editor Debugger panel: errors/warnings reported by a running
	// game (which are delivered over the debugger protocol and are NOT seen
	// by the in-process error handler used by logs_read).
	EditorDebuggerNode *edn = EditorDebuggerNode::get_singleton();
	Array out;
	for (int i = 0; i < 32; i++) { // Sessions are a small fixed set of tabs.
		ScriptEditorDebugger *dbg = edn->get_debugger(i);
		if (!dbg) {
			break;
		}
		Dictionary session;
		session["session"] = i;
		session["active"] = dbg->is_session_active();
		session["error_count"] = dbg->get_error_count();
		session["warning_count"] = dbg->get_warning_count();
		Array entries;
		Tree *tree = dbg->get_errors_tree();
		if (tree && tree->get_root()) {
			TreeItem *item = tree->get_root()->get_first_child();
			while (item) {
				Dictionary e;
				e["level"] = item->has_meta("_is_warning") ? "warning" : "error";
				e["time"] = item->get_text(0);
				e["message"] = item->get_text(1);
				Array details;
				TreeItem *child = item->get_first_child();
				while (child) {
					String t = child->get_text(0);
					if (!t.is_empty()) {
						details.append(t);
					}
					child = child->get_next();
				}
				if (!details.is_empty()) {
					e["details"] = details;
				}
				entries.append(e);
				item = item->get_next();
			}
		}
		session["entries"] = entries;
		out.append(session);
	}
	return mcp_tool_ret_json(out);
}

static Variant _tool_refresh(const Dictionary &p_args) {
	// Rescan the project filesystem and reload any scenes / project settings
	// that changed on disk, without restarting the editor.
	EditorNode *en = EditorNode::get_singleton();
	if (!en) {
		return Dictionary{ { "ok", false }, { "message", "Editor tidak siap" } };
	}
	en->refresh_external_changes();
	return Dictionary{ { "ok", true }, { "message", "Proyek disegarkan: pemindaian filesystem dijadwalkan; scene dan pengaturan proyek dimuat ulang dari disk." } };
}

static Dictionary _schema(bool p_required, const Vector<String> &p_props) {
	Dictionary props;
	for (const String &p : p_props) {
		props[p] = Dictionary{ { "type", "string" } };
	}
	Dictionary s;
	s["type"] = "object";
	s["properties"] = props;
	if (p_required) {
		Array req;
		for (const String &p : p_props) {
			req.append(p);
		}
		s["required"] = req;
	}
	return s;
}

static Dictionary _schema_any(const Vector<String> &p_optional) {
	Dictionary props;
	for (const String &p : p_optional) {
		props[p] = Dictionary{ { "type", "string" } };
	}
	Dictionary s;
	s["type"] = "object";
	s["properties"] = props;
	return s;
}

void mcp_register_tools(McpServer *p_server) {
	// Capture engine errors/warnings for logs_read.
	static bool log_hooks_installed = false;
	if (!log_hooks_installed) {
		log_hooks_installed = true;
		s_mcp_err_handler.errfunc = _mcp_log_err_cb;
		add_error_handler(&s_mcp_err_handler);
	}

	// godot-ai compatible core tools.
	p_server->register_tool("editor_state", "Ambil status editor: kesiapan (importing|playing|no_scene|ready), status bermain, scene aktif, dan url server.", _schema_any(Vector<String>()), _tool_editor_state);
	p_server->register_tool("server_info", "Alias dari editor_state.", _schema_any(Vector<String>()), _tool_editor_state);
	p_server->register_tool("session_activate", "Aktifkan/laporkan session editor saat ini. Args: tidak ada.", _schema_any(Vector<String>()), _tool_session_activate);
	p_server->register_tool("session_manage", "Informasi session. Args: op (info|activate).", _schema_any(Vector<String>{ "op" }), _tool_session_manage);

	// Scene tools.
	p_server->register_tool("scene_get_hierarchy", "Ambil pohon scene saat ini sebagai JSON (nodes, tipe, path, properti utama). Args: tidak ada.", _schema_any(Vector<String>()), _tool_get_scene_tree);
	p_server->register_tool("get_scene_tree", "Alias dari scene_get_hierarchy.", _schema_any(Vector<String>()), _tool_get_scene_tree);
	p_server->register_tool("scene_open", "Buka scene .tscn di editor. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_open_scene);
	p_server->register_tool("open_scene", "Alias dari scene_open.", _schema(true, Vector<String>{ "path" }), _tool_open_scene);
	p_server->register_tool("scene_save", "Simpan scene saat ini. Args: path (opsional, simpan sebagai).", _schema_any(Vector<String>{ "path" }), _tool_save_scene);
	p_server->register_tool("save_scene", "Alias dari scene_save.", _schema_any(Vector<String>{ "path" }), _tool_save_scene);
	p_server->register_tool("scene_manage", "Operasi scene. Args: op (create), path, root_class, root_name.", _schema(true, Vector<String>{ "op" }), _tool_scene_manage);
	p_server->register_tool("create_scene", "Alias dari scene_manage op=create. Args: path, root_class, root_name.", _schema(true, Vector<String>{ "path" }), _tool_create_scene);

	// Node tools.
	p_server->register_tool("node_create", "Tambahkan node ke scene. Args: class, name, parent (path relatif).", _schema(true, Vector<String>{ "class" }), _tool_add_node);
	p_server->register_tool("add_node", "Alias dari node_create.", _schema(true, Vector<String>{ "class" }), _tool_add_node);
	p_server->register_tool("node_manage", "Operasi node. Args: op (remove|rename|reparent), path, name, new_parent.", _schema(true, Vector<String>{ "op", "path" }), _tool_node_manage);
	p_server->register_tool("remove_node", "Alias dari node_manage op=remove. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_remove_node);
	p_server->register_tool("rename_node", "Alias dari node_manage op=rename. Args: path, name.", _schema(true, Vector<String>{ "path", "name" }), _tool_rename_node);
	p_server->register_tool("reparent_node", "Alias dari node_manage op=reparent. Args: path, new_parent.", _schema(true, Vector<String>{ "path", "new_parent" }), _tool_reparent_node);
	p_server->register_tool("node_get_properties", "Baca properti node. Args: path, property.", _schema(true, Vector<String>{ "path", "property" }), _tool_get_node_property);
	p_server->register_tool("get_node_property", "Alias dari node_get_properties.", _schema(true, Vector<String>{ "path", "property" }), _tool_get_node_property);
	p_server->register_tool("node_set_property", "Atur properti node (dapat dibatalkan). Args: path, property, value.", _schema(true, Vector<String>{ "path", "property" }), _tool_set_node_property);
	p_server->register_tool("set_node_property", "Alias dari node_set_property.", _schema(true, Vector<String>{ "path", "property" }), _tool_set_node_property);

	// Script tools.
	p_server->register_tool("script_create", "Tulis file GDScript. Args: path, content.", _schema(true, Vector<String>{ "path", "content" }), _tool_write_script);
	p_server->register_tool("write_script", "Alias dari script_create.", _schema(true, Vector<String>{ "path", "content" }), _tool_write_script);
	p_server->register_tool("script_attach", "Lampirkan script ke node (dibuat otomatis jika belum ada). Args: path (node), script (path).", _schema(true, Vector<String>{ "path", "script" }), _tool_attach_script);
	p_server->register_tool("attach_script", "Alias dari script_attach.", _schema(true, Vector<String>{ "path", "script" }), _tool_attach_script);
	p_server->register_tool("script_patch", "Terapkan patch find/replace pada script. Args: path, find, replace, all (bool, ganti semua kemunculan).", _schema(true, Vector<String>{ "path", "find", "replace" }), _tool_script_patch);
	p_server->register_tool("script_manage", "Operasi script. Args: op (read), path.", _schema(true, Vector<String>{ "op", "path" }), _tool_read_script);
	p_server->register_tool("read_script", "Alias dari script_manage op=read. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_read_script);

	// Filesystem tools.
	p_server->register_tool("filesystem_manage", "Operasi filesystem. Args: op (read|write|list), path, content, pattern, recursive.", _schema(true, Vector<String>{ "op" }), _tool_filesystem_manage);
	p_server->register_tool("read_file", "Alias dari filesystem_manage op=read. Args: path, json.", _schema(true, Vector<String>{ "path" }), _tool_read_file);
	p_server->register_tool("write_file", "Alias dari filesystem_manage op=write. Args: path, content.", _schema(true, Vector<String>{ "path", "content" }), _tool_write_file);
	p_server->register_tool("list_assets", "Alias dari filesystem_manage op=list. Args: pattern, recursive.", _schema_any(Vector<String>{ "pattern", "recursive" }), _tool_list_assets);

	// Project & run tools.
	p_server->register_tool("project_manage", "Operasi proyek. Args: op (info|get_setting|set_setting), name, value, save.", _schema(true, Vector<String>{ "op" }), _tool_project_manage);
	p_server->register_tool("project_info", "Alias dari project_manage op=info.", _schema_any(Vector<String>()), _tool_project_info);
	p_server->register_tool("get_project_setting", "Alias dari project_manage op=get_setting. Args: name.", _schema(true, Vector<String>{ "name" }), _tool_get_project_setting);
	p_server->register_tool("set_project_setting", "Alias dari project_manage op=set_setting. Args: name, value, save.", _schema(true, Vector<String>{ "name" }), _tool_set_project_setting);
	p_server->register_tool("project_run", "Jalankan/hentikan proyek. Args: op (play|run_scene|stop|state), path.", _schema(true, Vector<String>{ "op" }), _tool_project_run);
	p_server->register_tool("run_main_scene", "Alias dari project_run op=play.", _schema_any(Vector<String>()), _tool_run_main_scene);
	p_server->register_tool("run_custom_scene", "Alias dari project_run op=run_scene. Args: path.", _schema(true, Vector<String>{ "path" }), _tool_run_custom_scene);
	p_server->register_tool("stop_game", "Alias dari project_run op=stop.", _schema_any(Vector<String>()), _tool_stop_game);
	p_server->register_tool("game_state", "Alias dari project_run op=state.", _schema_any(Vector<String>()), _tool_game_state);

	// Game input.
	p_server->register_tool("game_manage", "Operasi game. Args: op (state|send_action|send_key|send_mouse|input), action/key/button, pressed, position, kind.", _schema(true, Vector<String>{ "op" }), _tool_game_manage);
	p_server->register_tool("send_input", "Alias dari game_manage op=input. Args: kind (action|key|mouse_button), action/key/button, pressed, position.", _schema(false, Vector<String>{ "kind", "action", "key", "button", "pressed", "position" }), _tool_send_input);

	// Editor utilities.
	p_server->register_tool("editor_screenshot", "Ambil screenshot viewport editor (atau game saat sedang berjalan). Mengembalikan gambar PNG. Args: source (editor|game|2d).", _schema_any(Vector<String>{ "source" }), _tool_screenshot);
	p_server->register_tool("screenshot", "Alias dari editor_screenshot.", _schema_any(Vector<String>{ "source" }), _tool_screenshot);
	p_server->register_tool("batch_execute", "Jalankan beberapa tool dalam satu kali perjalanan. Args: operations (array berisi {tool: nama, arguments: {}}), stop_on_error (bool). Mengembalikan array hasil.", _schema_any(Vector<String>{ "operations", "stop_on_error" }), _tool_batch_execute);
	p_server->register_tool("logs_read", "Baca baris log error/peringatan/MCP editor terbaru. Args: level (all|error|warning|info), limit (int).", _schema_any(Vector<String>{ "level", "limit" }), _tool_logs_read);
	p_server->register_tool("debugger_errors", "Baca error/peringatan yang sedang tampil di panel Debugger editor (dari game yang sedang berjalan).", _schema_any(Vector<String>()), _tool_debugger_errors);
	p_server->register_tool("refresh", "Pindai ulang filesystem proyek dan muat ulang scene/pengaturan proyek yang berubah di disk, tanpa memulai ulang editor.", _schema_any(Vector<String>()), _tool_refresh);
}

#endif // TOOLS_ENABLED