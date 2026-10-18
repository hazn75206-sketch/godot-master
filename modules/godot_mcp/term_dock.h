#ifndef TERM_DOCK_H
#define TERM_DOCK_H

#ifdef TOOLS_ENABLED

#include "editor/plugins/editor_plugin.h"
#include "core/input/input_event.h"
#include "core/variant/callable.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/control.h"
#include "scene/gui/line_edit.h"
#include "core/os/thread.h"
#include "core/templates/vector.h"

// Satu sel grid terminal.
struct TermCell {
	char32_t ch = U' ';
	Color fg = Color(0.85, 0.85, 0.85, 1);
	Color bg = Color(0.08, 0.08, 0.1, 1);
	bool bold = false;
	bool reverse = false;
};

// View grid terminal: parser VT100 subset + renderer.
// Berjalan di thread utama (data dipompa via call_deferred dari reader thread).
class TermView : public Control {
	GDCLASS(TermView, Control);

public:
	enum ParseState {
		PS_GROUND,
		PS_UTF8,
		PS_ESC,
		PS_CSI,
		PS_OSC,
		PS_CHARSET,
	};

private:
	Vector<Vector<TermCell>> lines; // buffer utama (+scrollback di depan)
	Vector<Vector<TermCell>> alt_lines; // layar alternatif (vim/htop)
	bool use_alt = false;
	bool alt_saved_cursor = false;
	int saved_cx = 0, saved_cy = 0;

	int cols = 80;
	int rows = 24;
	int cx = 0, cy = 0;
	int margin_top = 0, margin_bottom = 23;
	int scrollback_max = 500;
	int view_offset = 0; // 0 = ikuti bawah

	ParseState parse_state = PS_GROUND;

	Color cur_fg;
	Color cur_bg;
	bool cur_bold = false;
	bool cur_reverse = false;

	// Parser CSI.
	String csi_params;
	bool csi_priv = false; // '?' prefix

	// Decoder UTF-8.
	uint32_t utf_acc = 0;
	int utf_need = 0;

	// OSC skip.
	bool osc_esc = false;

	static Color palette[16];
	void _reset_attrs();

	Vector<Vector<TermCell>> &_buf();
	int _base(); // indeks baris pertama layar dalam buffer
	void _ensure_line(int y);
	void _put_char(char32_t ch);
	void _newline();
	void _scroll_up(int top, int bottom, int n);
	void _erase_display(int mode);
	void _erase_line(int mode);
	void _csi_dispatch(char final);
	int _csi_arg(int idx, int def);
	void _sgr();
	Color _color_from_idx(int idx);
	void _reply(const String &s);
	void _update_geometry();

protected:
	void _notification(int p_what);
	void _on_gui_input(const Ref<InputEvent> &p_event);

public:
	Callable reply_cb; // dipanggil parser untuk balasan terminal (String)
	Callable winsize_cb; // dipanggil saat ukuran grid berubah (cols, rows)
	void feed_bytes(const uint8_t *data, size_t len);
	void feed_byte(uint8_t b);
	void feed_char(char32_t ch) { _put_char(ch); }
	void clear_all();
	void scroll_view(int lines);
	int get_cols() const { return cols; }
	int get_rows() const { return rows; }
	TermView();
};

// Dock terminal: TermView + input + tombol kontrol + backend PTY/pipe.
class TermDock : public VBoxContainer {
	GDCLASS(TermDock, VBoxContainer);

	TermView *view = nullptr;
	LineEdit *input = nullptr;
	Button *btn_int = nullptr;
	Button *btn_esc = nullptr;
	Button *btn_tab = nullptr;
	Button *btn_clear = nullptr;
	Button *btn_restart = nullptr;

	// Backend shell.
	int fd_in = -1; // tulis ke shell (pty: sama dengan fd_out)
	int fd_out = -1; // baca dari shell
	int64_t child_pid = -1;
	bool use_pty = false;
	bool exiting = false;
	Thread reader_thread;
	Vector<String> history;
	int hist_idx = -1;

	static void _reader_static(void *ud);
	void _reader_loop();
	void _send(const String &s);
	void _send_str(const String &s);
	void _apply_winsize(int p_cols, int p_rows);
	void _send_bytes(const uint8_t *d, size_t n);
	void _spawn();
	void _kill_child();
	String _project_dir() const;

	void _on_submit(const String &text);
	void _on_int();
	void _on_esc();
	void _on_tab();
	void _on_clear();
	void _on_restart();
	void _append_data(const PackedByteArray &bytes);
	void _on_child_exit();

protected:
	static void _bind_methods();
	void _notification(int p_what) override;

public:
	TermDock();
	~TermDock();
};

class TermDockPlugin : public EditorPlugin {
	GDCLASS(TermDockPlugin, EditorPlugin);

	TermDock *dock = nullptr;
	EditorDock *editor_dock = nullptr;

	void _show_terminal();

public:
	TermDockPlugin();
	~TermDockPlugin();
};

#endif // TOOLS_ENABLED

#endif // TERM_DOCK_H
