/* OpenTTD's screen in a Nano-X window (docs/games/openttd.md): the 8-bit blitter draws into a
 * paletted texture in GPU memory, which the GPU draws over the whole window. One thread: the
 * game's ticks and the drawing take turns, and a wait for the next one also waits for input. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../palette_func.h"
#include "../blitter/factory.hpp"
#include "../core/geometry_func.hpp"
#include "../core/math_func.hpp"
#include "../framerate_type.h"
#include "../progress.h"
#include "../window_func.h"
#include "../timer/timer_game_tick.h"
#include "../network/network.h"
#include "shaderemu_v.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}

#include "../safeguards.h"

static FVideoDriver_ShaderEmu iFVideoDriver_ShaderEmu;

static GR_WINDOW_ID _window;
static GLuint _texture;
static uint8_t *_picture;          ///< the screen's pixels, in GPU memory: the texture
static int _picture_w, _picture_h; ///< the most it holds
static int _screen_cols, _screen_rows;
static GR_EVENT _pending;          ///< an event that ended a wait, handled next
static bool _have_pending;
static int _draws;                 ///< pictures drawn, for OPENTTD_RATE
static bool _drawn;                ///< a list is with the GPU: settle before writing the picture
static int _pointer_x, _pointer_y; ///< where the pointer was last, for moves while OpenTTD holds it
static unsigned _keys_down;        ///< bit 0-3 the arrows (as _dirkeys), 4 Tab
static GR_KEYMOD _modifiers;

#define EVENTS (GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_EXPOSURE | \
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

std::optional<std::string_view> VideoDriver_ShaderEmu::Start(const StringList &)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 8) return "only the 8-bit blitters";
	if (GrOpen() < 0) return "no Nano-X server (start one: nano-X -p &)";

	GR_SCREEN_INFO info;
	GrGetScreenInfo(&info);
	_screen_cols = info.cols;
	_screen_rows = info.rows;
	if (_cur_resolution.width == 0 || _cur_resolution.height == 0) _cur_resolution = { 640, 480 };
	_cur_resolution.width = Clamp<uint>(_cur_resolution.width, 640, _screen_cols);
	_cur_resolution.height = Clamp<uint>(_cur_resolution.height, 480, _screen_rows);

	_window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "OpenTTD", GR_ROOT_WINDOW_ID, -1, -1, _cur_resolution.width,
			_cur_resolution.height, 0);
	GrSelectEvents(_window, EVENTS);
	/* OpenTTD draws its own pointer, the tool's picture: the window system's is hidden over it */
	static const GR_BITMAP none[16] = {};
	GrSetCursor(_window, 16, 16, 0, 0, 0, 0, const_cast<GR_BITMAP *>(none), const_cast<GR_BITMAP *>(none));
	GrMapWindow(_window);
	GrSetFocus(_window);
	if (seglInit(_window) < 0) return "this machine has no GPU";

	/* The game loads for a minute at 2 MIPS without a picture: the window says so, drawn at each
	 * exposure of its first second, which is also the window manager's time to frame it. */
	GR_GC_ID gc = GrNewGC();
	GrSetGCForeground(gc, GR_RGB(224, 224, 224));
	GrSetGCBackground(gc, GR_RGB(0, 0, 0));
	for (auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1); std::chrono::steady_clock::now() < until;) {
		GR_EVENT event;
		GrGetNextEventTimeout(&event, 100);
		if (event.type == GR_EVENT_TYPE_EXPOSURE) GrText(_window, gc, 16, 24, const_cast<char *>("Loading OpenTTD..."), -1, GR_TFASCII);
	}
	GrDestroyGC(gc);

	_picture_w = std::max(_screen_cols, 640);
	_picture_h = std::max(_screen_rows, 480);
	_picture = static_cast<uint8_t *>(seglMemory(_picture_w * _picture_h));
	if (_picture == nullptr) return "no GPU memory for the screen";
	glGenTextures(1, &_texture);
	if (!this->Resize(_cur_resolution.width, _cur_resolution.height)) return "no screen";

	MarkWholeScreenDirty();
	this->is_game_threaded = false;
	return std::nullopt;
}

void VideoDriver_ShaderEmu::Stop()
{
	if (_drawn) seglSettle();
	GrClose();
}

/** The picture is w x h pixels from now on: its rows are as long as it is wide. */
bool VideoDriver_ShaderEmu::Resize(int w, int h)
{
	w = Clamp(w, 64, _picture_w);
	h = Clamp(h, 64, _picture_h);
	if (_drawn) seglSettle(), _drawn = false;

	glBindTexture(GL_TEXTURE_2D, _texture);
	seglTexturePointer(_picture, w, h, GL_COLOR_INDEX8_EXT);
	_screen.width = w;
	_screen.height = h;
	_screen.pitch = w;
	_screen.dst_ptr = _picture;
	_cur_resolution = { static_cast<uint>(w), static_cast<uint>(h) };
	this->dirty_rect = {};

	CopyPalette(this->local_palette, true);
	BlitterFactory::GetCurrentBlitter()->PostResize();
	GameSizeChanged();
	return true;
}

void VideoDriver_ShaderEmu::MakeDirty(int left, int top, int width, int height)
{
	Rect r = { left, top, left + width, top + height };
	this->dirty_rect = BoundingRect(this->dirty_rect, r);
}

void VideoDriver_ShaderEmu::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

bool VideoDriver_ShaderEmu::LockVideoBuffer()
{
	if (_drawn) seglSettle(), _drawn = false;
	_screen.dst_ptr = _picture;
	return false;
}

void VideoDriver_ShaderEmu::UnlockVideoBuffer() {}

/** The picture over the whole window, submitted and not waited for: the next lock settles. */
void VideoDriver_ShaderEmu::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);
	static const GLfixed ONE = 65536;
	static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
	static const GLfixed xyz[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 };
	static const GLfixed uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };

	if (IsEmptyRect(this->dirty_rect) && this->local_palette.count_dirty == 0) return;
	if (_drawn) seglSettle(), _drawn = false;
	_draws++;

	if (this->local_palette.count_dirty != 0) {
		unsigned *words = seglPalette();
		for (int i = this->local_palette.first_dirty; i < this->local_palette.first_dirty + this->local_palette.count_dirty; i++) {
			const Colour &c = this->local_palette.palette[i];
			words[i] = static_cast<unsigned>(c.r) << 16 | static_cast<unsigned>(c.g) << 8 | c.b;
		}
		this->local_palette.count_dirty = 0;
	}

	int width, height;
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(xyz, uv, _texture, 255, 255, 0);
	seglSwapEarly();
	_drawn = true;
	this->dirty_rect = {};
}

/* A key as OpenTTD numbers it (WKC_*), and the character it types into *character */
static uint KeyOf(const GR_EVENT_KEYSTROKE &k, char32_t *character)
{
	static const char shifted[] = ")!@#$%^&*(";
	GR_KEY ch = k.ch;
	uint key = 0;
	bool printable = ch >= ' ' && ch < 0x7f;

	if (ch >= 'a' && ch <= 'z') key = ch - 'a' + 'A';
	else if (ch >= 'A' && ch <= 'Z') key = ch;
	else if (ch >= '0' && ch <= '9') key = ch;
	else if (ch >= MWKEY_F1 && ch <= MWKEY_F10) key = WKC_F1 + (ch - MWKEY_F1);
	else if (ch >= MWKEY_KP0 && ch <= MWKEY_KP9) key = '0' + (ch - MWKEY_KP0), printable = true, ch = key;
	else if (const char *s = ch > 0 && ch < 0x80 ? strchr(shifted, ch) : nullptr) key = '0' + (s - shifted);
	else {
		switch (ch) {
			case MWKEY_F11: key = WKC_F11; break;
			case MWKEY_F12: key = WKC_F12; break;
			case MWKEY_PAGEUP: key = WKC_PAGEUP; break;
			case MWKEY_PAGEDOWN: key = WKC_PAGEDOWN; break;
			case MWKEY_UP: key = WKC_UP; break;
			case MWKEY_DOWN: key = WKC_DOWN; break;
			case MWKEY_LEFT: key = WKC_LEFT; break;
			case MWKEY_RIGHT: key = WKC_RIGHT; break;
			case MWKEY_HOME: key = WKC_HOME; break;
			case MWKEY_END: key = WKC_END; break;
			case MWKEY_INSERT: key = WKC_INSERT; break;
			case MWKEY_DELETE: case 127: key = WKC_DELETE; printable = false; break;
			case MWKEY_ESCAPE: key = WKC_ESC; printable = false; break;
			case MWKEY_PAUSE: key = WKC_PAUSE; break;
			case MWKEY_BACKSPACE: key = WKC_BACKSPACE; break;
			case ' ': key = WKC_SPACE; break;
			case MWKEY_ENTER: case '\n': key = WKC_RETURN; break;
			case MWKEY_TAB: key = WKC_TAB; break;
			case MWKEY_KP_DIVIDE: key = WKC_NUM_DIV; break;
			case MWKEY_KP_MULTIPLY: key = WKC_NUM_MUL; break;
			case MWKEY_KP_MINUS: key = WKC_NUM_MINUS; break;
			case MWKEY_KP_PLUS: key = WKC_NUM_PLUS; break;
			case MWKEY_KP_ENTER: key = WKC_NUM_ENTER; break;
			case MWKEY_KP_PERIOD: key = WKC_NUM_DECIMAL; break;
			case '/': case '?': key = WKC_SLASH; break;
			case ';': case ':': key = WKC_SEMICOLON; break;
			case '=': case '+': key = WKC_EQUALS; break;
			case '[': case '{': key = WKC_L_BRACKET; break;
			case '\\': case '|': key = WKC_BACKSLASH; break;
			case ']': case '}': key = WKC_R_BRACKET; break;
			case '\'': case '"': key = WKC_SINGLEQUOTE; break;
			case ',': case '<': key = WKC_COMMA; break;
			case '-': case '_': key = WKC_MINUS; break;
			case '.': case '>': key = WKC_PERIOD; break;
			case '`': case '~': key = WKC_BACKQUOTE; break;
		}
	}
	if (k.modifiers & MWKMOD_META) key |= WKC_META;
	if (k.modifiers & MWKMOD_SHIFT) key |= WKC_SHIFT;
	if (k.modifiers & MWKMOD_CTRL) key |= WKC_CTRL;
	if (k.modifiers & MWKMOD_ALT) key |= WKC_ALT;
	*character = printable && !(k.modifiers & (MWKMOD_META | MWKMOD_CTRL | MWKMOD_ALT)) ? static_cast<char32_t>(ch) : WKC_NONE;
	return key;
}

static unsigned HeldBit(GR_KEY ch)
{
	switch (ch) {
		case MWKEY_LEFT: return 1;
		case MWKEY_UP: return 2;
		case MWKEY_RIGHT: return 4;
		case MWKEY_DOWN: return 8;
		case MWKEY_TAB: return 16;
		default: return 0;
	}
}

static void PointerAt(int x, int y)
{
	if (_cursor.fix_at) {
		/* OpenTTD holds the pointer still while the right button drags the view: the moves count */
		_cursor.UpdateCursorPositionRelative(x - _pointer_x, y - _pointer_y);
	} else {
		_cursor.UpdateCursorPosition(x, y);
	}
	_pointer_x = x;
	_pointer_y = y;
}

bool VideoDriver_ShaderEmu::PollEvent()
{
	GR_EVENT e;

	if (_have_pending) {
		e = _pending;
		_have_pending = false;
	} else {
		GrCheckNextEvent(&e);
	}
	switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			return false;

		case GR_EVENT_TYPE_MOUSE_MOTION:
			_modifiers = e.mouse.modifiers;
			PointerAt(e.mouse.x, e.mouse.y);
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_BUTTON_DOWN:
			_modifiers = e.button.modifiers;
			PointerAt(e.button.x, e.button.y);
			if (e.button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) {
				int notch = (e.button.changebuttons & GR_BUTTON_SCROLLUP) ? -1 : 1;
				_cursor.wheel += notch;
				_cursor.v_wheel += notch * 14.0f * _settings_client.gui.scrollwheel_multiplier;
				_cursor.wheel_moved = true;
				break;
			}
			if (e.button.changebuttons & GR_BUTTON_L) {
				if (_rightclick_emulate && (e.button.modifiers & MWKMOD_CTRL)) {
					_right_button_down = true;
					_right_button_clicked = true;
				} else {
					_left_button_down = true;
				}
			}
			if (e.button.changebuttons & GR_BUTTON_R) {
				_right_button_down = true;
				_right_button_clicked = true;
			}
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_BUTTON_UP:
			_modifiers = e.button.modifiers;
			PointerAt(e.button.x, e.button.y);
			if (e.button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) break;
			if (_rightclick_emulate) {
				_right_button_down = false;
				_left_button_down = false;
				_left_button_clicked = false;
			} else {
				if (e.button.changebuttons & GR_BUTTON_L) _left_button_down = false, _left_button_clicked = false;
				if (e.button.changebuttons & GR_BUTTON_R) _right_button_down = false;
			}
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_MOUSE_ENTER:
			_cursor.in_window = true;
			break;

		case GR_EVENT_TYPE_MOUSE_EXIT:
			UndrawMouseCursor();
			_cursor.in_window = false;
			break;

		case GR_EVENT_TYPE_KEY_DOWN: {
			char32_t character;
			_modifiers = e.keystroke.modifiers;
			_keys_down |= HeldBit(e.keystroke.ch);
			uint key = KeyOf(e.keystroke, &character);
			if (key != 0 || character != WKC_NONE) HandleKeypress(key, character);
			break;
		}

		case GR_EVENT_TYPE_KEY_UP:
			_modifiers = e.keystroke.modifiers;
			_keys_down &= ~HeldBit(e.keystroke.ch);
			break;

		case GR_EVENT_TYPE_EXPOSURE:
			this->MakeDirty(0, 0, _screen.width, _screen.height);
			break;

		case GR_EVENT_TYPE_UPDATE:
			if (e.update.wid == _window && e.update.utype == GR_UPDATE_SIZE) {
				seglWindowChanged();
				if (e.update.width != _screen.width || e.update.height != _screen.height) this->Resize(e.update.width, e.update.height);
			}
			break;

		case GR_EVENT_TYPE_CLOSE_REQ:
			HandleExitGameRequest();
			break;
	}
	return true;
}

void VideoDriver_ShaderEmu::InputLoop()
{
	bool old_ctrl_pressed = _ctrl_pressed;

	_ctrl_pressed = (_modifiers & MWKMOD_CTRL) != 0;
	_shift_pressed = (_modifiers & MWKMOD_SHIFT) != 0;
	/* Tab held runs the game fast, as on other systems */
	this->fast_forward_key_pressed = (_keys_down & 16) && !(_modifiers & MWKMOD_ALT);
	_dirkeys = _keys_down & 15;
	/* OPENTTD_SCROLL=1, for measuring: the arrows held down and right, then up and left, two seconds each */
	static const bool scroll_test = getenv("OPENTTD_SCROLL") != nullptr;
	if (scroll_test && _game_mode == GM_NORMAL) {
		auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		_dirkeys |= (s / 2) % 2 ? 4 | 8 : 1 | 2;
	}
	/* OPENTTD_HOSTING=FILE: "openttd" written there once this server's game runs (nxgame, docs/system/lan.md) */
	static const char *hosting = getenv("OPENTTD_HOSTING");
	if (hosting != nullptr && _network_server && _game_mode == GM_NORMAL) {
		if (FILE *f = fopen(hosting, "w"); f != nullptr) fmt::print(f, "openttd\n"), fclose(f);
		hosting = nullptr;
	}

	if (old_ctrl_pressed != _ctrl_pressed) HandleCtrlChanged();
}

void VideoDriver_ShaderEmu::MainLoop()
{
	/* OPENTTD_RATE=1: pictures drawn and game ticks run, every ten seconds */
	bool rate = getenv("OPENTTD_RATE") != nullptr;
	auto rate_from = std::chrono::steady_clock::now();
	uint64_t ticks_from = TimerGameTick::counter;
	while (!_exit_game) {
		this->Tick();
		if (rate && std::chrono::steady_clock::now() - rate_from >= std::chrono::seconds(10)) {
			fmt::print(stderr, "openttd: {} draws, {} ticks in 10 s\n", _draws, TimerGameTick::counter - std::min(ticks_from, TimerGameTick::counter));
			rate_from += std::chrono::seconds(10);
			ticks_from = TimerGameTick::counter;
			_draws = 0;
		}

		/* until the next tick or draw, or an event */
		auto next = std::min(this->next_draw_tick, this->next_game_tick);
		auto now = std::chrono::steady_clock::now();
		if (next > now && !_have_pending) {
			long ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
			if (ms > 0) {
				GrGetNextEventTimeout(&_pending, ms);
				_have_pending = _pending.type != GR_EVENT_TYPE_NONE && _pending.type != GR_EVENT_TYPE_TIMEOUT;
			}
		}
	}
}

bool VideoDriver_ShaderEmu::ChangeResolution(int w, int h)
{
	GrResizeWindow(_window, w, h);
	return this->Resize(w, h);
}

bool VideoDriver_ShaderEmu::ToggleFullscreen(bool)
{
	return false;
}

bool VideoDriver_ShaderEmu::AfterBlitterChange()
{
	return BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 8 && this->Resize(_screen.width, _screen.height);
}

Dimension VideoDriver_ShaderEmu::GetScreenSize() const
{
	return { static_cast<uint>(_screen_cols), static_cast<uint>(_screen_rows) };
}
