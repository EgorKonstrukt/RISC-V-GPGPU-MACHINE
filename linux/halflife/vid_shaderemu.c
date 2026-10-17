/*
vid_shaderemu.c - the window, its frames and its input on the machine (docs/halflife.md)

A Nano-X window drawn by the machine's GPU through gl_shaderemu.c, which ref/gl finds its
OpenGL in. Keys and the pointer come from the window system's events.
*/
#include "platform/platform.h"
#if XASH_VIDEO == VIDEO_SHADEREMU
#include "input.h"
#include "client.h"
#include "vid_common.h"
#include <nano-X.h>

int hlglOpen( unsigned int nano_x_window );
void hlglSwap( void );
void hlglWindowChanged( int *width, int *height );
void *hlglProcAddress( const char *name );

static GR_WINDOW_ID window;
static int opened, win_w = 640, win_h = 480;
static int mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_known, text_input;

void Platform_Minimize_f( void ) { }

void GL_SwapBuffers( void )
{
	if( opened )
		hlglSwap();
}

static qboolean open_window( void )
{
	GR_SCREEN_INFO info;
	string size;

	if( opened )
		return true;
	if( GrOpen() < 0 )
	{
		Con_Printf( S_ERROR "no Nano-X server (start one: nano-X -p &)\n" );
		return false;
	}
	GrGetScreenInfo( &info );
	if( Sys_GetParmFromCmdLine( "-width", size ))
		win_w = Q_atoi( size );
	if( Sys_GetParmFromCmdLine( "-height", size ))
		win_h = Q_atoi( size );
	if( win_w > info.cols ) win_w = info.cols;
	if( win_h > info.rows ) win_h = info.rows;
	window = GrNewWindowEx( GR_WM_PROPS_APPWINDOW, "Half-Life: Uplink", GR_ROOT_WINDOW_ID, -1, -1, win_w, win_h, 0 );
	GrSelectEvents( window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT |
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE );
	GrMapWindow( window );
	GrSetFocus( window );
	if( hlglOpen( window ) < 0 )
	{
		Con_Printf( S_ERROR "this machine has no GPU\n" );
		return false;
	}
	opened = 1;
	return true;
}

qboolean R_Init_Video( ref_graphic_apis_t type )
{
	if( type != REF_GL || !open_window( ))
		return false;
	if( !VID_SetMode( ))
		return false;
	ref.dllFuncs.GL_InitExtensions();
	host.renderinfo_changed = false;
	return true;
}

void R_Free_Video( void )
{
	ref.dllFuncs.GL_ClearExtensions();
}

qboolean VID_SetMode( void )
{
	R_ChangeDisplaySettings( win_w, win_h, WINDOW_MODE_WINDOWED );
	return true;
}

rserr_t R_ChangeDisplaySettings( int width, int height, window_mode_t window_mode )
{
	int render_w = win_w, render_h = win_h;

	VID_SetDisplayTransform( &render_w, &render_h );
	R_SaveVideoMode( win_w, win_h, render_w, render_h, false );
	return rserr_ok;
}

int GL_SetAttribute( int attr, int val ) { return 0; }
int GL_GetAttribute( int attr, int *val )
{
	*val = attr == REF_GL_RED_SIZE || attr == REF_GL_GREEN_SIZE || attr == REF_GL_BLUE_SIZE ? 8 : attr == REF_GL_CONTEXT_MAJOR_VERSION ? 1 : 0;
	return 0;
}
int R_MaxVideoModes( void ) { return 0; }
vidmode_t *R_GetVideoMode( int num ) { return NULL; }
void *GL_GetProcAddress( const char *name ) { return hlglProcAddress( name ); }
void GL_UpdateSwapInterval( void ) { ClearBits( gl_vsync.flags, FCVAR_CHANGED ); }
void *SW_LockBuffer( void ) { return NULL; }
void SW_UnlockBuffer( void ) { }
qboolean SW_CreateBuffer( int width, int height, uint *stride, uint *bpp, uint *r, uint *g, uint *b ) { return false; }
ref_window_type_t R_GetWindowHandle( void **handle, ref_window_type_t type ) { return REF_WINDOW_TYPE_NULL; }
void VID_Info_f( void ) { Con_Printf( "Nano-X window %dx%d, the machine's GPU\n", win_w, win_h ); }
platform_orientation_t Platform_GetDisplayOrientation( void ) { return ORIENTATION_LANDSCAPE; }

/* ---- input ---- */

static int engine_key( int ch )
{
	static const struct { int ch, key; } other[] = {
		{ MWKEY_LEFT, K_LEFTARROW }, { MWKEY_RIGHT, K_RIGHTARROW }, { MWKEY_UP, K_UPARROW }, { MWKEY_DOWN, K_DOWNARROW },
		{ MWKEY_ENTER, K_ENTER }, { '\n', K_ENTER }, { MWKEY_KP_ENTER, K_KP_ENTER }, { MWKEY_ESCAPE, K_ESCAPE },
		{ MWKEY_BACKSPACE, K_BACKSPACE }, { MWKEY_TAB, K_TAB }, { MWKEY_INSERT, K_INS }, { MWKEY_DELETE, K_DEL },
		{ MWKEY_HOME, K_HOME }, { MWKEY_END, K_END }, { MWKEY_PAGEUP, K_PGUP }, { MWKEY_PAGEDOWN, K_PGDN },
		{ MWKEY_LSHIFT, K_SHIFT }, { MWKEY_RSHIFT, K_SHIFT }, { MWKEY_LCTRL, K_CTRL }, { MWKEY_RCTRL, K_CTRL },
		{ MWKEY_LALT, K_ALT }, { MWKEY_RALT, K_ALT }, { MWKEY_PAUSE, K_PAUSE },
	};
	static const char shifted[] = ")!@#$%^&*(";
	const char *at;
	size_t i;

	if( ch >= 'A' && ch <= 'Z' )
		return ch - 'A' + 'a';
	if( ch >= MWKEY_F1 && ch <= MWKEY_F12 )
		return K_F1 + ch - MWKEY_F1;
	for( i = 0; i < ARRAYSIZE( other ); i++ )
		if( other[i].ch == ch )
			return other[i].key;
	if( ch > 0 && ch < 128 && ( at = Q_strchr( shifted, ch )) != NULL )
		return '0' + (int)( at - shifted );
	return ch >= 32 && ch < 127 ? ch : 0;
}

void Platform_RunEvents( void )
{
	GR_EVENT e;
	int key, down;

	if( !opened )
		return;
	for( ;; )
	{
		GrCheckNextEvent( &e );
		switch( e.type )
		{
		case GR_EVENT_TYPE_NONE:
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_known = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_POSITION:
			if( mouse_known )
				mouse_dx += e.mouse.x - mouse_x, mouse_dy += e.mouse.y - mouse_y;
			mouse_x = e.mouse.x, mouse_y = e.mouse.y, mouse_known = 1;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			mouse_x = e.button.x, mouse_y = e.button.y;
			if( e.button.changebuttons & GR_BUTTON_L ) IN_MouseEvent( 0, down );
			if( e.button.changebuttons & GR_BUTTON_R ) IN_MouseEvent( 1, down );
			if( e.button.changebuttons & GR_BUTTON_M ) IN_MouseEvent( 2, down );
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			key = engine_key( e.keystroke.ch );
			if( key )
				Key_Event( key, down );
			if( down && text_input && e.keystroke.ch >= 32 && e.keystroke.ch < 127 )
				CL_CharEvent( e.keystroke.ch );
			break;
		case GR_EVENT_TYPE_UPDATE:
			if( e.update.utype == GR_UPDATE_SIZE )
			{
				int w = 0, h = 0;
				hlglWindowChanged( &w, &h );
				if( w > 0 && h > 0 && ( w != win_w || h != win_h ))
					win_w = w, win_h = h, R_ChangeDisplaySettings( w, h, WINDOW_MODE_WINDOWED );
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Sys_Quit( "window closed" );
			break;
		}
	}
}

void Platform_MouseMove( float *x, float *y )
{
	*x = (float)mouse_dx, *y = (float)mouse_dy;
	mouse_dx = mouse_dy = 0;
}

void Platform_EnableTextInput( qboolean enable, int x, int y, int w, int h ) { text_input = enable; }
void Platform_PreCreateMove( void ) { }
void GAME_EXPORT Platform_GetMousePos( int *x, int *y )
{
	if( x ) *x = mouse_x * refState.scale_x;
	if( y ) *y = mouse_y * refState.scale_y;
}
void GAME_EXPORT Platform_SetMousePos( int x, int y ) { }
qboolean Platform_GetMouseGrab( void ) { return false; }
void Platform_SetMouseGrab( qboolean enable ) { }
void Platform_SetCursorType( VGUI_DefaultCursor type ) { }
int Platform_GetClipboardText( char *buffer, size_t size ) { return 0; }
void Platform_SetClipboardText( const char *buffer ) { }
#endif
