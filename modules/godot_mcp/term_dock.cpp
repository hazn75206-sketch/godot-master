#include "term_dock.h"

#ifdef TOOLS_ENABLED

#include "editor/editor_node.h"
#include "core/config/project_settings.h"
#include "core/variant/callable.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "scene/gui/box_container.h"
#include "scene/gui/line_edit.h"
#include "scene/main/window.h"

#include <string.h>

#if defined(UNIX_ENABLED)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#if __has_include(<pty.h>)
#include <pty.h>
#define TERM_HAS_PTY 1
#elif __has_include(<util.h>)
#include <util.h>
#define TERM_HAS_PTY 1
#else
#define TERM_HAS_PTY 0
#endif
#else
#define TERM_HAS_PTY 0
#endif

// ---------------------------------------------------------------- palette ---

Color TermView::palette[16] = {
	Color(0.1, 0.1, 0.1, 1), Color(0.75, 0.2, 0.2, 1),
	Color(0.3, 0.7, 0.3, 1), Color(0.75, 0.65, 0.2, 1),
	Color(0.3, 0.4, 0.85, 1), Color(0.65, 0.3, 0.7, 1),
	Color(0.3, 0.65, 0.7, 1), Color(0.8, 0.8, 0.8, 1),
	Color(0.4, 0.4, 0.4, 1), Color(1.0, 0.35, 0.35, 1),
	Color(0.45, 0.9, 0.45, 1), Color(1.0, 0.9, 0.4, 1),
	Color(0.45, 0.55, 1.0, 1), Color(0.85, 0.45, 0.95, 1),
	Color(0.45, 0.9, 0.9, 1), Color(1.0, 1.0, 1.0, 1),
};

// ---------------------------------------------------------------- TermView ---

TermView::TermView() {
	cur_fg = palette[7];
	cur_bg = palette[0];
	lines.resize(rows);
	for (int i = 0; i < rows; i++) {
		lines.write[i].resize(cols);
	}
	margin_bottom = rows - 1;
	set_focus_mode(FOCUS_ALL);
	set_mouse_filter(MOUSE_FILTER_STOP);
	connect("gui_input", callable_mp(this, &TermView::_on_gui_input));
}

void TermView::_on_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid() && mb->is_pressed()) {
		if (mb->get_button_index() == MouseButton::WHEEL_UP) {
			scroll_view(3);
			accept_event();
		} else if (mb->get_button_index() == MouseButton::WHEEL_DOWN) {
			scroll_view(-3);
			accept_event();
		} else {
			view_offset = 0;
			queue_redraw();
		}
	}
}

Vector<Vector<TermCell>> &TermView::_buf() {
	return use_alt ? alt_lines : lines;
}

// Buffer = scrollback + tepat `rows` baris layar di ekor (invarian).
// cy/cx/margin disimpan relatif layar; fungsi ini konversi basisnya.
int TermView::_base() {
	int s = (int)_buf().size() - rows;
	return s < 0 ? 0 : s;
}

void TermView::_reset_attrs() {
	cur_fg = palette[7];
	cur_bg = palette[0];
	cur_bold = false;
	cur_reverse = false;
}

void TermView::_ensure_line(int y) {
	Vector<Vector<TermCell>> &b = _buf();
	while (b.size() <= y) {
		Vector<TermCell> nl;
		nl.resize(cols);
		b.push_back(nl);
	}
	if (b[y].size() < cols) {
		int old = b[y].size();
		b.write[y].resize(cols);
		for (int i = old; i < cols; i++) {
			b.write[y].write[i] = TermCell();
		}
	}
}

void TermView::_put_char(char32_t ch) {
	Vector<Vector<TermCell>> &b = _buf();
	if (cy < margin_top) {
		cy = margin_top;
	}
	if (cy > margin_bottom) {
		cy = margin_bottom;
	}
	if (cx >= cols) {
		cx = 0;
		cy++;
		if (cy > margin_bottom) {
			_scroll_up(margin_top, margin_bottom, 1);
			cy = margin_bottom;
		}
	}
	int ay = _base() + cy;
	_ensure_line(ay);
	TermCell c;
	c.ch = ch;
	c.fg = cur_bold && cur_fg == palette[7] ? palette[15] : cur_fg;
	c.bg = cur_bg;
	c.bold = cur_bold;
	c.reverse = cur_reverse;
	b.write[ay].write[cx] = c;
	cx++;
	if (cx >= cols) {
		cx = 0;
		cy++;
		if (cy > margin_bottom) {
			_scroll_up(margin_top, margin_bottom, 1);
			cy = margin_bottom;
		}
	}
	view_offset = 0;
}

void TermView::_newline() {
	cx = 0;
	cy++;
	if (cy > margin_bottom) {
		_scroll_up(margin_top, margin_bottom, 1);
		cy = margin_bottom;
	}
	view_offset = 0;
}

void TermView::_scroll_up(int top, int bottom, int n) {
	Vector<Vector<TermCell>> &b = _buf();
	if (n <= 0) {
		return;
	}
	int base = _base();
	bool full = (!use_alt && top == 0 && bottom == rows - 1);
	for (int k = 0; k < n; k++) {
		if (full) {
			// Baris teratas layar jadi scrollback; blank baru di ekor.
			b.remove_at(base);
			while ((int)b.size() > rows + scrollback_max) {
				b.remove_at(0);
			}
			Vector<TermCell> nl;
			nl.resize(cols);
			b.push_back(nl);
		} else {
			for (int y = top; y < bottom; y++) {
				b.write[base + y] = b[base + y + 1];
			}
			Vector<TermCell> nl;
			nl.resize(cols);
			b.write[base + bottom] = nl;
		}
	}
}

void TermView::_erase_display(int mode) {
	Vector<Vector<TermCell>> &b = _buf();
	if (mode == 3 && !use_alt) {
		while ((int)b.size() > rows) {
			b.remove_at(0);
		}
		mode = 2;
	}
	int base = _base();
	if (mode == 2) {
		for (int y = 0; y < rows; y++) {
			for (int x = 0; x < cols; x++) {
				{ TermCell _nc; _nc.bg = cur_bg; b.write[base + y].write[x] = _nc; }
			}
		}
	} else if (mode == 0) {
		for (int x = cx; x < cols; x++) {
			{ TermCell _nc; _nc.bg = cur_bg; b.write[base + cy].write[x] = _nc; }
		}
		for (int y = cy + 1; y < rows; y++) {
			for (int x = 0; x < cols; x++) {
				{ TermCell _nc; _nc.bg = cur_bg; b.write[base + y].write[x] = _nc; }
			}
		}
	} else if (mode == 1) {
		for (int y = 0; y < cy; y++) {
			for (int x = 0; x < cols; x++) {
				{ TermCell _nc; _nc.bg = cur_bg; b.write[base + y].write[x] = _nc; }
			}
		}
		for (int x = 0; x <= cx && x < cols; x++) {
			{ TermCell _nc; _nc.bg = cur_bg; b.write[base + cy].write[x] = _nc; }
		}
	}
}

void TermView::_erase_line(int mode) {
	Vector<Vector<TermCell>> &b = _buf();
	int ay = _base() + cy;
	if (mode == 2) {
		for (int x = 0; x < cols; x++) {
			{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[x] = _nc; }
		}
	} else if (mode == 0) {
		for (int x = cx; x < cols; x++) {
			{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[x] = _nc; }
		}
	} else if (mode == 1) {
		for (int x = 0; x <= cx && x < cols; x++) {
			{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[x] = _nc; }
		}
	}
}

int TermView::_csi_arg(int idx, int def) {
	int start = 0;
	int cur = 0;
	for (int i = 0; i <= (int)csi_params.length(); i++) {
		if (i == (int)csi_params.length() || csi_params[i] == ';') {
			if (cur == idx) {
				if (i == start) {
					return def;
				}
				return csi_params.substr(start, i - start).to_int();
			}
			cur++;
			start = i + 1;
		}
	}
	return def;
}

Color TermView::_color_from_idx(int idx) {
	if (idx < 0 || idx > 255) {
		return palette[7];
	}
	if (idx < 16) {
		return palette[idx];
	}
	if (idx < 232) {
		int v = idx - 16;
		int r = v / 36, g = (v % 36) / 6, bl = v % 6;
		return Color(r / 5.0f, g / 5.0f, bl / 5.0f, 1);
	}
	float g = (idx - 232) / 23.0f;
	return Color(g, g, g, 1);
}

void TermView::_sgr() {
	// Kumpulkan argumen (kosong = 0).
	Vector<int> args;
	int start = 0;
	bool any = false;
	for (int i = 0; i <= (int)csi_params.length(); i++) {
		if (i == (int)csi_params.length() || csi_params[i] == ';') {
			any = true;
			if (i == start) {
				args.push_back(0);
			} else {
				args.push_back(csi_params.substr(start, i - start).to_int());
			}
			start = i + 1;
		}
	}
	if (!any) {
		args.push_back(0);
	}
	for (int i = 0; i < (int)args.size(); i++) {
		int a = args[i];
		if (a == 0) {
			_reset_attrs();
		} else if (a == 1) {
			cur_bold = true;
			if (cur_fg == palette[7]) {
				cur_fg = palette[15];
			}
		} else if (a == 7) {
			cur_reverse = true;
		} else if (a == 22) {
			cur_bold = false;
		} else if (a == 27) {
			cur_reverse = false;
		} else if (a >= 30 && a <= 37) {
			cur_fg = palette[a - 30];
			if (cur_bold) {
				cur_fg = palette[a - 30 + 8];
			}
		} else if (a >= 90 && a <= 97) {
			cur_fg = palette[a - 90 + 8];
		} else if (a == 39) {
			cur_fg = palette[7];
		} else if (a >= 40 && a <= 47) {
			cur_bg = palette[a - 40];
		} else if (a >= 100 && a <= 107) {
			cur_bg = palette[a - 100 + 8];
		} else if (a == 49) {
			cur_bg = palette[0];
		} else if ((a == 38 || a == 48) && i + 1 < (int)args.size()) {
			bool is_fg = (a == 38);
			if (args[i + 1] == 5 && i + 2 < (int)args.size()) {
				Color c = _color_from_idx(args[i + 2]);
				if (is_fg) {
					cur_fg = c;
				} else {
					cur_bg = c;
				}
				i += 2;
			} else if (args[i + 1] == 2 && i + 4 < (int)args.size()) {
				Color c(args[i + 2] / 255.0f, args[i + 3] / 255.0f, args[i + 4] / 255.0f, 1);
				if (is_fg) {
					cur_fg = c;
				} else {
					cur_bg = c;
				}
				i += 4;
			}
		}
	}
}

void TermView::_csi_dispatch(char final) {
	Vector<Vector<TermCell>> &b = _buf();
	switch (final) {
		case 'A': { // CUU
			cy = MAX(margin_top, cy - MAX(1, _csi_arg(0, 1)));
		} break;
		case 'B': { // CUD
			cy = MIN(margin_bottom, cy + MAX(1, _csi_arg(0, 1)));
			_ensure_line(_base() + cy);
		} break;
		case 'C': { // CUF
			cx = MIN(cols - 1, cx + MAX(1, _csi_arg(0, 1)));
		} break;
		case 'D': { // CUB
			cx = MAX(0, cx - MAX(1, _csi_arg(0, 1)));
		} break;
		case 'E': {
			cx = 0;
			cy = MIN(margin_bottom, cy + MAX(1, _csi_arg(0, 1)));
			_ensure_line(_base() + cy);
		} break;
		case 'F': {
			cx = 0;
			cy = MAX(margin_top, cy - MAX(1, _csi_arg(0, 1)));
		} break;
		case 'G': {
			cx = CLAMP(_csi_arg(0, 1) - 1, 0, cols - 1);
		} break;
		case 'H':
		case 'f': {
			int r = _csi_arg(0, 1) - 1;
			int c = _csi_arg(1, 1) - 1;
			cy = CLAMP(r, margin_top, margin_bottom);
			cx = CLAMP(c, 0, cols - 1);
			_ensure_line(_base() + cy);
		} break;
		case 'd': {
			cy = CLAMP(_csi_arg(0, 1) - 1, margin_top, margin_bottom);
			_ensure_line(_base() + cy);
		} break;
		case 'J': {
			_erase_display(_csi_arg(0, 0));
		} break;
		case 'K': {
			_erase_line(_csi_arg(0, 0));
		} break;
		case 'L': { // IL: sisip baris
			int n = MAX(1, _csi_arg(0, 1));
			int base = _base();
			for (int k = 0; k < n; k++) {
				for (int y = margin_bottom; y > cy; y--) {
					b.write[base + y] = b[base + y - 1];
				}
				Vector<TermCell> nl;
				nl.resize(cols);
				b.write[base + cy] = nl;
			}
		} break;
		case 'M': { // DL: hapus baris
			int n = MAX(1, _csi_arg(0, 1));
			int base = _base();
			for (int k = 0; k < n; k++) {
				for (int y = cy; y < margin_bottom; y++) {
					b.write[base + y] = b[base + y + 1];
				}
				Vector<TermCell> nl;
				nl.resize(cols);
				b.write[base + margin_bottom] = nl;
			}
		} break;
		case '@': { // ICH: sisip kolom
			int n = MAX(1, _csi_arg(0, 1));
			int ay = _base() + cy;
			for (int k = 0; k < n; k++) {
				for (int x = cols - 1; x > cx; x--) {
					b.write[ay].write[x] = b[ay][x - 1];
				}
				{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[cx] = _nc; }
			}
		} break;
		case 'P': { // DCH: hapus kolom
			int n = MAX(1, _csi_arg(0, 1));
			int ay = _base() + cy;
			for (int k = 0; k < n; k++) {
				for (int x = cx; x < cols - 1; x++) {
					b.write[ay].write[x] = b[ay][x + 1];
				}
				{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[cols - 1] = _nc; }
			}
		} break;
		case 'X': { // ECH: hapus n kolom
			int n = MAX(1, _csi_arg(0, 1));
			int ay = _base() + cy;
			for (int x = cx; x < MIN(cols, cx + n); x++) {
				{ TermCell _nc; _nc.bg = cur_bg; b.write[ay].write[x] = _nc; }
			}
		} break;
		case 'S': { // SU: scroll naik
			_scroll_up(margin_top, margin_bottom, MAX(1, _csi_arg(0, 1)));
		} break;
		case 'T': { // SD: scroll turun
			int n = MAX(1, _csi_arg(0, 1));
			int base = _base();
			for (int k = 0; k < n; k++) {
				for (int y = margin_bottom; y > margin_top; y--) {
					b.write[base + y] = b[base + y - 1];
				}
				Vector<TermCell> nl;
				nl.resize(cols);
				b.write[base + margin_top] = nl;
			}
		} break;
		case 'c': { // DA
			_reply("\x1b[?62c");
		} break;
		case 'n': { // DSR
			if (_csi_arg(0, 0) == 6) {
				_reply(vformat("\x1b[%d;%dR", cy + 1, cx + 1));
			}
		} break;
		case 'r': { // DECSTBM: margin scroll
			int t = _csi_arg(0, 1) - 1;
			int bo = _csi_arg(1, 9999) - 1;
			margin_top = CLAMP(t, 0, rows - 1);
			margin_bottom = CLAMP(bo, 0, rows - 1);
			if (margin_top >= margin_bottom) {
				margin_top = 0;
				margin_bottom = rows - 1;
			}
			cx = 0;
			cy = margin_top;
		} break;
		case 's': {
			saved_cx = cx;
			saved_cy = cy;
		} break;
		case 'u': {
			cx = CLAMP(saved_cx, 0, cols - 1);
			cy = CLAMP(saved_cy, 0, rows - 1);
			_ensure_line(_base() + cy);
		} break;
		case 'm': {
			_sgr();
		} break;
		case 'h':
		case 'l': {
			bool set = (final == 'h');
			// Layar alternatif (1049/1047) + abaikan mode lain.
			bool alt = csi_params.find("1049") >= 0 || csi_params.find("1047") >= 0;
			if (alt) {
				if (set && !use_alt) {
					saved_cx = cx;
					saved_cy = cy;
					alt_saved_cursor = true;
					use_alt = true;
					alt_lines.clear();
					alt_lines.resize(rows);
					for (int i = 0; i < rows; i++) {
						alt_lines.write[i].resize(cols);
					}
					cx = 0;
					cy = 0;
					margin_top = 0;
					margin_bottom = rows - 1;
				} else if (!set && use_alt) {
					use_alt = false;
					if (alt_saved_cursor) {
						cx = CLAMP(saved_cx, 0, cols - 1);
						cy = CLAMP(saved_cy, 0, rows - 1);
					}
				}
			}
		} break;
		default: {
		} break;
	}
}

void TermView::_reply(const String &s) {
	if (reply_cb.is_valid()) {
		reply_cb.call(s);
	}
}

void TermView::feed_byte(uint8_t b) {
	switch (parse_state) {
		case PS_GROUND: {
			if (b == 0x1b) {
				parse_state = PS_ESC;
			} else if (b == 0x07) {
				// BEL: abaikan.
			} else if (b == 0x08) {
				cx = MAX(0, cx - 1);
			} else if (b == 0x09) {
				cx = MIN(cols - 1, (cx / 8 + 1) * 8);
			} else if (b == 0x0a || b == 0x0b || b == 0x0c) {
				_newline();
			} else if (b == 0x0d) {
				cx = 0;
			} else if (b < 0x20 || b == 0x7f) {
				// Kontrol lain: abaikan.
			} else if (b < 0x80) {
				_put_char((char32_t)b);
				queue_redraw();
			} else {
				// UTF-8 multi-byte.
				if ((b & 0xe0) == 0xc0) {
					utf_acc = b & 0x1f;
					utf_need = 1;
				} else if ((b & 0xf0) == 0xe0) {
					utf_acc = b & 0x0f;
					utf_need = 2;
				} else if ((b & 0xf8) == 0xf0) {
					utf_acc = b & 0x07;
					utf_need = 3;
				} else {
					utf_need = 0;
					break;
				}
				parse_state = PS_UTF8;
			}
		} break;
		case PS_UTF8: {
			if ((b & 0xc0) == 0x80 && utf_need > 0) {
				utf_acc = (utf_acc << 6) | (b & 0x3f);
				utf_need--;
				if (utf_need == 0) {
					_put_char((char32_t)utf_acc);
					queue_redraw();
					parse_state = PS_GROUND;
				}
			} else {
				utf_need = 0;
				parse_state = PS_GROUND;
				feed_byte(b);
			}
		} break;
		case PS_ESC: {
			if (b == '[') {
				csi_params = "";
				csi_priv = false;
				parse_state = PS_CSI;
			} else if (b == ']') {
				osc_esc = false;
				parse_state = PS_OSC;
			} else if (b == '(' || b == ')') {
				parse_state = PS_CHARSET;
			} else if (b == '7') {
				saved_cx = cx;
				saved_cy = cy;
				parse_state = PS_GROUND;
			} else if (b == '8') {
				cx = CLAMP(saved_cx, 0, cols - 1);
				cy = CLAMP(saved_cy, 0, rows - 1);
				parse_state = PS_GROUND;
			} else if (b == 'M') {
				if (cy <= margin_top) {
					// Reverse index: geser bawah dalam margin.
					Vector<Vector<TermCell>> &bb = _buf();
					int base = _base();
					for (int y = margin_bottom; y > margin_top; y--) {
						bb.write[base + y] = bb[base + y - 1];
					}
					Vector<TermCell> nl;
					nl.resize(cols);
					bb.write[base + margin_top] = nl;
				} else {
					cy--;
				}
				parse_state = PS_GROUND;
			} else if (b == 'E') {
				_newline();
				parse_state = PS_GROUND;
			} else if (b == 'c') {
				clear_all();
				parse_state = PS_GROUND;
			} else if (b == '=' || b == '>') {
				parse_state = PS_GROUND;
			} else if (b == 'P' || b == '_' || b == '^') {
				osc_esc = false;
				parse_state = PS_OSC;
			} else {
				parse_state = PS_GROUND;
			}
		} break;
		case PS_CSI: {
			if (b == '?' && csi_params.is_empty()) {
				csi_priv = true;
			} else if ((b >= '0' && b <= '9') || b == ';' || b == ':' || b == '$' || b == ' ') {
				csi_params += String::chr(b);
			} else if (b >= 0x40 && b <= 0x7e) {
				_csi_dispatch((char)b);
				queue_redraw();
				parse_state = PS_GROUND;
			} else {
				parse_state = PS_GROUND;
			}
		} break;
		case PS_OSC: {
			if (osc_esc) {
				if (b == '\\') {
					parse_state = PS_GROUND;
				} else {
					osc_esc = false;
				}
			} else if (b == 0x1b) {
				osc_esc = true;
			} else if (b == 0x07) {
				parse_state = PS_GROUND;
			}
		} break;
		case PS_CHARSET: {
			parse_state = PS_GROUND;
		} break;
	}
}

void TermView::feed_bytes(const uint8_t *data, size_t len) {
	for (size_t i = 0; i < len; i++) {
		feed_byte(data[i]);
	}
	queue_redraw();
}

void TermView::clear_all() {
	lines.clear();
	lines.resize(rows);
	for (int i = 0; i < rows; i++) {
		lines.write[i].resize(cols);
	}
	alt_lines.clear();
	use_alt = false;
	cx = 0;
	cy = 0;
	margin_top = 0;
	margin_bottom = rows - 1;
	_reset_attrs();
	view_offset = 0;
	queue_redraw();
}

void TermView::scroll_view(int p_lines) {
	int max_off = MAX(0, (int)lines.size() - rows);
	view_offset = CLAMP(view_offset + p_lines, 0, max_off);
	queue_redraw();
}

void TermView::_update_geometry() {
	Ref<Font> font = get_theme_default_font();
	int fsize = get_theme_default_font_size();
	float adv = font->get_string_size("MMMMMMMMMM", HORIZONTAL_ALIGNMENT_LEFT, -1, fsize).x / 10.0f;
	float lh = (float)font->get_height(fsize);
	if (adv < 1.0f) {
		adv = 8.0f;
	}
	if (lh < 1.0f) {
		lh = 16.0f;
	}
	int ncols = MAX(20, (int)(get_size().x / adv));
	int nrows = MAX(5, (int)(get_size().y / lh));
	if (ncols == cols && nrows == rows) {
		return;
	}
	cols = ncols;
	rows = nrows;
	margin_top = 0;
	margin_bottom = rows - 1;
	cx = MIN(cx, cols - 1);
	cy = MIN(cy, rows - 1);
	// Jaga invarian: buffer >= rows baris.
	while ((int)lines.size() < rows) {
		Vector<TermCell> nl;
		nl.resize(cols);
		lines.push_back(nl);
		cy++;
		cy = MIN(cy, rows - 1);
	}
	while ((int)lines.size() > rows + scrollback_max) {
		lines.remove_at(0);
	}
	for (int i = 0; i < (int)lines.size(); i++) {
		if ((int)lines[i].size() < cols) {
			int old = lines[i].size();
			lines.write[i].resize(cols);
			for (int k = old; k < cols; k++) {
				lines.write[i].write[k] = TermCell();
			}
		}
	}
	alt_lines.clear();
	use_alt = false;
	if (winsize_cb.is_valid()) {
		winsize_cb.call(cols, rows);
	}
	queue_redraw();
}

void TermView::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		_update_geometry();
	} else if (p_what == NOTIFICATION_THEME_CHANGED) {
		queue_redraw();
	} else if (p_what == NOTIFICATION_DRAW) {
		Ref<Font> font = get_theme_default_font();
		int fsize = get_theme_default_font_size();
		float adv = font->get_string_size("MMMMMMMMMM", HORIZONTAL_ALIGNMENT_LEFT, -1, fsize).x / 10.0f;
		float lh = (float)font->get_height(fsize);
		if (adv < 1.0f) {
			adv = 8.0f;
		}
		if (lh < 1.0f) {
			lh = 16.0f;
		}
		float ascent = (float)font->get_ascent(fsize);
		draw_rect(Rect2(Point2(), get_size()), palette[0]);
		Vector<Vector<TermCell>> &b = _buf();
		int total = (int)b.size();
		int bottom = total - view_offset;
		for (int r = 0; r < rows; r++) {
			int li = bottom - rows + r;
			float y = r * lh;
			if (li < 0 || li >= total) {
				continue;
			}
			const Vector<TermCell> &line = b[li];
			int x = 0;
			while (x < cols) {
				int x0 = x;
				Color bg = (x < (int)line.size()) ? line[x].bg : palette[0];
				while (x < cols && (x >= (int)line.size() || line[x].bg == bg)) {
					x++;
				}
				if (!(bg == palette[0])) {
					draw_rect(Rect2(x0 * adv, y, (x - x0) * adv, lh), bg);
				}
			}
			// Foreground per run warna sama.
			String run;
			Color run_fg = palette[7];
			float rx = 0;
			for (int c = 0; c < cols; c++) {
				char32_t ch = U' ';
				Color fg = palette[7];
				if (c < (int)line.size()) {
					ch = line[c].ch;
					fg = line[c].reverse ? line[c].bg : line[c].fg;
				}
				if (run.is_empty()) {
					run_fg = fg;
					rx = c * adv;
				}
				if (!(fg == run_fg)) {
					if (!run.is_empty()) {
						draw_string(font, Point2(rx, y + ascent), run, HORIZONTAL_ALIGNMENT_LEFT, -1, fsize, run_fg);
					}
					run = "";
					run_fg = fg;
					rx = c * adv;
				}
				if (ch != U' ') {
					run += String::chr(ch);
				} else {
					run += " ";
				}
			}
			if (!run.is_empty()) {
				draw_string(font, Point2(rx, y + ascent), run, HORIZONTAL_ALIGNMENT_LEFT, -1, fsize, run_fg);
			}
		}
		// Kursor.
		{
			int vis_cy = cy - (bottom - rows);
			if (vis_cy >= 0 && vis_cy < rows) {
				draw_rect(Rect2(cx * adv, vis_cy * lh, adv, lh), Color(0.7, 0.9, 1.0, 0.7), false, 2.0f);
			}
		}
	}
}

// ---------------------------------------------------------------- TermDock ---

TermDock::TermDock() {
	set_name("Terminal");
}

TermDock::~TermDock() {
	_kill_child();
}

String TermDock::_project_dir() const {
	return ProjectSettings::get_singleton()->globalize_path("res://");
}

void TermDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_append_data", "bytes"), &TermDock::_append_data);
	ClassDB::bind_method(D_METHOD("_on_child_exit"), &TermDock::_on_child_exit);
}

void TermDock::_enter_tree() {
	view = memnew(TermView);
	view->set_custom_minimum_size(Vector2(0, 240));
	view->set_v_size_flags(SIZE_EXPAND_FILL);
	view->reply_cb = callable_mp(this, &TermDock::_send_str);
	view->winsize_cb = callable_mp(this, &TermDock::_apply_winsize);
	add_child(view);

	HBoxContainer *bar = memnew(HBoxContainer);
	add_child(bar);

	input = memnew(LineEdit);
	input->set_placeholder("perintah sh... (enter = jalan)");
	input->set_h_size_flags(SIZE_EXPAND_FILL);
	input->connect("text_submitted", callable_mp(this, &TermDock::_on_submit));
	bar->add_child(input);

	btn_int = memnew(Button);
	btn_int->set_text("Ctrl+C");
	btn_int->set_tooltip_text("Kirim SIGINT (Ctrl+C)");
	btn_int->connect("pressed", callable_mp(this, &TermDock::_on_int));
	bar->add_child(btn_int);

	btn_esc = memnew(Button);
	btn_esc->set_text("Esc");
	btn_esc->connect("pressed", callable_mp(this, &TermDock::_on_esc));
	bar->add_child(btn_esc);

	btn_tab = memnew(Button);
	btn_tab->set_text("Tab");
	btn_tab->connect("pressed", callable_mp(this, &TermDock::_on_tab));
	bar->add_child(btn_tab);

	btn_clear = memnew(Button);
	btn_clear->set_text("Clear");
	btn_clear->connect("pressed", callable_mp(this, &TermDock::_on_clear));
	bar->add_child(btn_clear);

	btn_restart = memnew(Button);
	btn_restart->set_text("Restart");
	btn_restart->set_tooltip_text("Matikan shell dan mulai baru");
	btn_restart->connect("pressed", callable_mp(this, &TermDock::_on_restart));
	bar->add_child(btn_restart);

	_spawn();
}

void TermDock::_exit_tree() {
	exiting = true;
	_kill_child();
}

void TermDock::_spawn() {
#if !defined(UNIX_ENABLED) || defined(_WIN32)
	view->clear_all();
	const char *msg = "Terminal butuh Unix (sh). Platform ini belum didukung.\n";
	view->feed_bytes((const uint8_t *)msg, strlen(msg));
	return;
#else
	_kill_child();
	exiting = false;
	String dir = _project_dir();
#if TERM_HAS_PTY
	struct winsize ws;
	ws.ws_col = (unsigned short)MAX(view->get_cols(), 20);
	ws.ws_row = (unsigned short)MAX(view->get_rows(), 5);
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;
	child_pid = forkpty(&fd_out, nullptr, nullptr, &ws);
	if (child_pid < 0) {
		const char *msg = "forkpty gagal, coba mode pipe.\n";
		view->feed_bytes((const uint8_t *)msg, strlen(msg));
	}
	if (child_pid == 0) {
		// Anak.
		chdir(dir.utf8().get_data());
		setenv("TERM", "xterm-256color", 1);
		const char *sh = nullptr;
#ifdef __ANDROID__
		sh = "/system/bin/sh";
#else
		sh = getenv("SHELL");
		if (!sh || !*sh) {
			sh = "/bin/sh";
		}
#endif
		const char *bn = strrchr(sh, '/');
		bn = bn ? bn + 1 : sh;
		execl(sh, bn, "-i", (char *)nullptr);
		_exit(127);
	}
	if (child_pid > 0) {
		fd_in = fd_out;
		use_pty = true;
		int flags = fcntl(fd_out, F_GETFL, 0);
		fcntl(fd_out, F_SETFL, flags | O_NONBLOCK);
		reader_thread.start(_reader_static, this);
		const char *msg = "# shell siap (pty). Direktori = project.\n";
		view->feed_bytes((const uint8_t *)msg, strlen(msg));
		return;
	}
	// Gagal pty -> pipe.
#endif
	// Mode pipe (fallback): sh -i dengan stdin/stdout terarah.
	int pin[2], pout[2];
	if (pipe(pin) < 0 || pipe(pout) < 0) {
		return;
	}
	child_pid = fork();
	if (child_pid == 0) {
		dup2(pin[0], STDIN_FILENO);
		dup2(pout[1], STDOUT_FILENO);
		dup2(pout[1], STDERR_FILENO);
		close(pin[0]);
		close(pin[1]);
		close(pout[0]);
		close(pout[1]);
		chdir(dir.utf8().get_data());
		setenv("TERM", "dumb", 1);
		const char *sh = getenv("SHELL");
		if (!sh || !*sh) {
			sh = "/bin/sh";
		}
		const char *bn = strrchr(sh, '/');
		bn = bn ? bn + 1 : sh;
		execl(sh, bn, "-i", (char *)nullptr);
		_exit(127);
	}
	close(pin[0]);
	close(pout[1]);
	fd_in = pin[1];
	fd_out = pout[0];
	use_pty = false;
	int flags = fcntl(fd_out, F_GETFL, 0);
	fcntl(fd_out, F_SETFL, flags | O_NONBLOCK);
	reader_thread.start(_reader_static, this);
	const char *msg = "# shell siap (pipe, tanpa pty). Direktori = project.\n";
	view->feed_bytes((const uint8_t *)msg, strlen(msg));
#endif
}

void TermDock::_kill_child() {
	exiting = true;
	if (child_pid > 0) {
#if defined(UNIX_ENABLED)
		kill((pid_t)child_pid, SIGKILL);
		int status = 0;
		waitpid((pid_t)child_pid, &status, 0); // SIGKILL pasti mati -> aman blokir
#endif
		child_pid = -1;
	}
	if (reader_thread.is_started()) {
		reader_thread.wait_to_finish();
	}
	if (fd_in >= 0 && fd_in != fd_out) {
#if defined(UNIX_ENABLED)
		close(fd_in);
#endif
	}
	if (fd_out >= 0) {
#if defined(UNIX_ENABLED)
		close(fd_out);
#endif
	}
	fd_in = -1;
	fd_out = -1;
}

void TermDock::_reader_static(void *ud) {
	((TermDock *)ud)->_reader_loop();
}

void TermDock::_reader_loop() {
	uint8_t buf[4096];
	while (!exiting) {
		if (fd_out < 0) {
			break;
		}
#if defined(UNIX_ENABLED)
		ssize_t n = read(fd_out, buf, sizeof(buf));
#else
		break;
#endif
		if (n > 0) {
			PackedByteArray bytes;
			bytes.resize(n);
			memcpy(bytes.ptrw(), buf, n);
			call_deferred("_append_data", bytes);
		} else if (n == 0) {
			call_deferred("_on_child_exit");
			break;
		} else {
#if defined(UNIX_ENABLED)
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				OS::get_singleton()->delay_usec(15000);
				continue;
			}
			if (errno == EIO) {
				// PTY tutup saat anak mati.
				call_deferred("_on_child_exit");
				break;
			}
#endif
			OS::get_singleton()->delay_usec(15000);
		}
	}
}

void TermDock::_append_data(const PackedByteArray &bytes) {
	if (view && bytes.size() > 0) {
		view->feed_bytes(bytes.ptr(), bytes.size());
	}
}

void TermDock::_on_child_exit() {
	if (exiting) {
		return;
	}
	child_pid = -1;
	const char *msg = "\n[shell berhenti — tekan Restart untuk mulai baru]\n";
	view->feed_bytes((const uint8_t *)msg, strlen(msg));
}

void TermDock::_send_bytes(const uint8_t *d, size_t n) {
	if (fd_in < 0 || n == 0) {
		return;
	}
#if defined(UNIX_ENABLED)
	size_t off = 0;
	while (off < n) {
		ssize_t w = write(fd_in, d + off, n - off);
		if (w < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		off += (size_t)w;
	}
#else
	(void)d;
	(void)n;
#endif
}

void TermDock::_send(const String &s) {
	CharString cs = s.utf8();
	_send_bytes((const uint8_t *)cs.get_data(), cs.length());
}

void TermDock::_send_str(const String &s) {
	_send(s);
}

void TermDock::_apply_winsize(int p_cols, int p_rows) {
#if defined(UNIX_ENABLED)
	if (!use_pty || fd_out < 0 || child_pid <= 0) {
		return;
	}
	struct winsize ws;
	ws.ws_col = (unsigned short)MAX(p_cols, 1);
	ws.ws_row = (unsigned short)MAX(p_rows, 1);
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;
	ioctl(fd_out, TIOCSWINSZ, &ws);
	kill((pid_t)child_pid, SIGWINCH);
#else
	(void)p_cols;
	(void)p_rows;
#endif
}

void TermDock::_on_submit(const String &text) {
	if (text.is_empty()) {
		return;
	}
	history.push_back(text);
	hist_idx = history.size();
	_send(text + "\n");
	input->clear();
}

void TermDock::_on_int() {
#if defined(UNIX_ENABLED)
	if (use_pty) {
		_send("\x03");
	} else if (child_pid > 0) {
		kill((pid_t)child_pid, SIGINT);
	}
#endif
}

void TermDock::_on_esc() {
	_send("\x1b");
}

void TermDock::_on_tab() {
	_send("\t");
}

void TermDock::_on_clear() {
	view->clear_all();
}

void TermDock::_on_restart() {
	view->clear_all();
	_spawn();
}

// ---------------------------------------------------------------- plugin ---

TermDockPlugin::TermDockPlugin() {
	dock = nullptr;
}

TermDockPlugin::~TermDockPlugin() {
}

void TermDockPlugin::_enter_tree() {
	dock = memnew(TermDock);
	add_control_to_bottom_panel(dock, "Terminal");
}

void TermDockPlugin::_exit_tree() {
	if (dock) {
		remove_control_from_bottom_panel(dock);
		dock = nullptr; // Editor yang mem-free panel.
	}
}

#endif // TOOLS_ENABLED
