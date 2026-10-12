/*
 * GLQuake's window, frame and input on the ShaderEmu machine (docs/quake.md): a Nano-X
 * window the GPU draws into, in place of gl_vidlinuxglx.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include "quakedef.h"
#include "nano-X.h"

#define CAPTION		22		/* the window manager's caption and frame (nanowm.h) */
#define FRAME		4
#define REG_INPUT	0x20		/* the machine's input counters (docs/input.md) */
#define REG_CLOCK	0x34
#define WARP_WIDTH	320
#define WARP_HEIGHT	200

unsigned	d_8to24table[256];
unsigned char	d_15to8table[65536];	/* for mipmaps of paletted textures, which are not made */

int		texture_mode = GL_NEAREST;
int		texture_extension_number = 1;
float		gldepthmin, gldepthmax;
cvar_t		gl_ztrick = {"gl_ztrick", "0"};
const char	*gl_vendor, *gl_renderer, *gl_version, *gl_extensions;
qboolean	isPermedia = false;
qboolean	gl_mtexable = false;

static cvar_t	in_mouse = {"in_mouse", "0", true};	/* the pointer turns the view */
static GR_WINDOW_ID window;
static int	opened;
static int	mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_known, mouse_inside = 1;
static unsigned	frames, hold_frame, hold_late, stats_ms = 5000, stats_frames;

void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height) {}
void D_EndDirectRect (int x, int y, int width, int height) {}
void VID_HandlePause (qboolean pause) {}
void VID_ShiftPalette (unsigned char *palette) {}
qboolean VID_Is8bit (void) { return true; }

void VID_Shutdown (void)
{
	if (opened)
		GrClose ();
	opened = 0;
}

/* The machine's clock in milliseconds once the GPU is ours: a load, where the system's is a call. */
unsigned VID_Milliseconds (void)
{
	return opened ? *qglControl (REG_CLOCK) : 0;
}

void VID_SetPalette (unsigned char *palette)
{
	unsigned	*table = d_8to24table, *words;
	int		i;

	for (i = 0; i < 256; i++, palette += 3)
		table[i] = 255u << 24 | palette[0] | palette[1] << 8 | (unsigned)palette[2] << 16;
	d_8to24table[255] &= 0xffffff;	/* 255 is transparent */
	if (!opened)
		return;
	words = seglPalette ();
	seglSettle ();
	for (i = 0; i < 256; i++)
		words[i] = (table[i] & 0xff) << 16 | (table[i] & 0xff00) | (table[i] >> 16 & 0xff);
}

void GL_Init (void)
{
	gl_vendor = (const char *)glGetString (GL_VENDOR);
	gl_renderer = (const char *)glGetString (GL_RENDERER);
	gl_version = (const char *)glGetString (GL_VERSION);
	gl_extensions = (const char *)glGetString (GL_EXTENSIONS);
	Con_Printf ("GL_RENDERER: %s\n", gl_renderer);

	glClearColor (0, 0, 0, 0);
	glEnable (GL_TEXTURE_2D);
	glEnable (GL_ALPHA_TEST);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* the host shows the world round a seat: what is beside and behind the camera is drawn too (docs/holodeck.md) */
int se_all_round;

void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	extern int sb_updates;
	const volatile unsigned *host = seglHostState ();

	*x = *y = 0;
	seglSize (width, height);
	se_all_round = host && SEGL_HOST_HOLODECK (host);
	sb_updates = 0;		/* every frame is drawn whole: the status bar too */
}

unsigned se_started;	/* the instruction count at main (sys_shaderemu.c) */

static unsigned cycles (void)
{
	unsigned v;

	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

/*
 * The part of a frame that begins here: 0 the rest, 1 the server, 2 finding what of the level
 * is in view, 3 drawing it, 4 models, 5 particles and the weapon, 6 the 2D screen and the swap.
 */
static unsigned phase, phase_since, phase_cycles[7];
extern unsigned se_server_cycles, se_server_frames, se_server_passes, se_server_between, se_server_held, se_server_behind;	/* server_shaderemu.c */
#ifdef SE_COUNT
unsigned se_count[10];	/* what the server did, counted where it does it (quake.patch) */
#endif

void VID_Phase (int next)
{
	unsigned now = cycles ();

	SE_ServerPoll ();	/* a worker core with the server's frame may want a page */
	phase_cycles[phase] += now - phase_since;
	phase = next;
	phase_since = now;
}

/*
 * Every few seconds: frames a second, and the machine's instructions a frame (quakestat:
 * lines; QUAKE_STATS_MS says how often, 0 never). QUAKE_HOLD=N keeps frame N on the screen
 * and stops there, which is how pictures are compared; with QUAKE_HOLD_LATE=1 it stops when
 * the frame after is ready to be shown, having written whatever that one writes.
 */
void GL_EndRendering (void)
{
	static unsigned since_ms, since_cycles, since_frames;
	static int level_shown;
	unsigned now;

	SE_FrameEnd ();
	if (hold_frame && hold_late && frames >= hold_frame) {
		hold_late = 0;
	} else {
		qglSwap ();
		VID_Phase (0);
		frames++;
	}
	if (hold_frame && frames >= hold_frame && !hold_late) {
		seglSettle ();	/* (the frame is on the screen when this says so) */
		printf ("quake: holding frame %u\n", frames);
		fflush (stdout);
		for (;;)
			__asm__ volatile (".word 0x0100000f");	/* pause: the machine's frame ends */
	}
	if (!level_shown && stats_ms && cls.signon == SIGNONS) {
		/* from the program's start to a level's first picture */
		level_shown = 1;
		printf ("quakestat: a level's first frame is frame %u, %u thousand instructions from the start\n", frames, (cycles () - se_started) / 1000);
		fflush (stdout);
	}
	now = VID_Milliseconds ();
	if (!since_ms)
		since_ms = now, since_cycles = cycles (), since_frames = frames;
	if (stats_frames ? frames - since_frames >= stats_frames : stats_ms && now - since_ms >= stats_ms) {
		unsigned n = frames - since_frames, kept, in_gpu, gpu_size, evicted, vertices;

		qglMemory (&kept, &in_gpu, &gpu_size, &evicted, &vertices);
		printf ("quakestat: %u frames in %u ms, %u instructions a frame, %u vertices; textures %u KB, %u of %u KB in the GPU, %u evicted\n",
			n, now - since_ms, n ? (cycles () - since_cycles) / n : 0, vertices, kept >> 10, in_gpu >> 10, gpu_size >> 10, evicted);
		if (n) {
			unsigned *c = phase_cycles;

			printf ("quakestat: a frame in thousands: server %u, in view %u, level %u, models %u, particles and weapon %u, 2D and swap %u, rest %u\n",
				c[1] / n / 1000, c[2] / n / 1000, c[3] / n / 1000, c[4] / n / 1000, c[5] / n / 1000, c[6] / n / 1000, c[0] / n / 1000);
			memset (phase_cycles, 0, sizeof phase_cycles);
			if (se_server_frames) {
				printf ("quakestat: the server on a worker core: %u frames of its own to these %u (%u drawn between two of them, %u standing at the newer, %u behind the older), %u thousand instructions each, %u.%u passes waited for one\n",
					se_server_frames, n, se_server_between, se_server_held, se_server_behind, se_server_cycles / se_server_frames / 1000, se_server_passes / se_server_frames, se_server_passes * 10 / se_server_frames % 10);
				se_server_cycles = se_server_frames = se_server_passes = se_server_between = se_server_held = se_server_behind = 0;
			}
#ifdef SE_COUNT
			printf ("quakestat: a frame's server: %u point tests of %u nodes, %u moves, %u point contents, %u links, %u QuakeC calls of %u statements; %u water and %u sky vertices\n",
				se_count[0] / n, se_count[1] / n, se_count[2] / n, se_count[3] / n, se_count[4] / n, se_count[5] / n, se_count[6] / n,
				se_count[8] / n, se_count[9] / n);
			memset (se_count, 0, sizeof se_count);
#endif
		}
		fflush (stdout);
		since_ms = now, since_cycles = cycles (), since_frames = frames;
	}
}

/* The console command "hold": the frame after next stays, as with QUAKE_HOLD. */
static void hold_command (void)
{
	hold_frame = frames + 2;
}

/* GLQuake's brightening of the palette: 0.7 unless -gamma says (1 leaves it as it is). */
static void gamma_palette (unsigned char *palette)
{
	float	gamma = 0.7, f;
	int	i;

	if ((i = COM_CheckParm ("-gamma")) != 0)
		gamma = Q_atof (com_argv[i + 1]);
	if (gamma == 1)
		return;
	if (gamma == 0.7f) {
		/* what the sums below come to for 0.7, by byte: they are 768 powers in doubles, a
		   twentieth of the time from the command to the first picture */
		static const unsigned char brighter[256] = {
	5, 9, 11, 14, 16, 18, 21, 23, 24, 26, 28, 30, 32, 33, 35, 37, 38, 40, 41, 43,
	44, 46, 47, 49, 50, 51, 53, 54, 56, 57, 58, 59, 61, 62, 63, 65, 66, 67, 68, 70,
	71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89, 90, 91, 92,
	93, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
	114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 147, 148, 149, 150,
	151, 152, 153, 154, 154, 155, 156, 157, 158, 159, 160, 160, 161, 162, 163, 164, 165, 165, 166, 167,
	168, 169, 170, 170, 171, 172, 173, 174, 175, 175, 176, 177, 178, 179, 179, 180, 181, 182, 183, 184,
	184, 185, 186, 187, 188, 188, 189, 190, 191, 191, 192, 193, 194, 195, 195, 196, 197, 198, 199, 199,
	200, 201, 202, 202, 203, 204, 205, 205, 206, 207, 208, 208, 209, 210, 211, 212, 212, 213, 214, 215,
	215, 216, 217, 218, 218, 219, 220, 221, 221, 222, 223, 223, 224, 225, 226, 226, 227, 228, 229, 229,
	230, 231, 232, 232, 233, 234, 234, 235, 236, 237, 237, 238, 239, 239, 240, 241, 242, 242, 243, 244,
	244, 245, 246, 247, 247, 248, 249, 249, 250, 251, 252, 252, 253, 254, 254, 255,
		};

		for (i = 0; i < 768; i++)
			palette[i] = brighter[palette[i]];
		return;
	}
	for (i = 0; i < 768; i++) {
		f = pow ((palette[i] + 1) / 256.0, gamma) * 255 + 0.5;
		palette[i] = f < 0 ? 0 : f > 255 ? 255 : (int)f;
	}
}

void VID_Init (unsigned char *palette)
{
	GR_SCREEN_INFO	info;
	int		i, width = 640, height = 480, bare;
	const char	*s;

	Cvar_RegisterVariable (&in_mouse);
	Cvar_RegisterVariable (&gl_ztrick);
	Cmd_AddCommand ("hold", hold_command);

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	if (GrOpen () < 0)
		Sys_Error ("no Nano-X server (start one: nano-X -p &)");
	GrGetScreenInfo (&info);
	if ((i = COM_CheckParm ("-width")) != 0)
		width = atoi (com_argv[i + 1]);
	if ((i = COM_CheckParm ("-height")) != 0)
		height = atoi (com_argv[i + 1]);
	if (width > info.cols)
		width = info.cols;
	if (height > info.rows)
		height = info.rows;
	/* a window that leaves no room for a frame and caption has none, and sits in the corner */
	bare = width + 2 * FRAME > info.cols || height + CAPTION + FRAME > info.rows;
	window = GrNewWindowEx (bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"Quake", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width, height, 0);
	GrSelectEvents (window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE);
	GrMapWindow (window);
	GrSetFocus (window);
	if (qglOpen (window, palette) < 0)
		Sys_Error ("this machine has no GPU");
	opened = 1;

	/* the 2D screen is half the window's size from 640 across, so that its text can be read */
	if ((i = COM_CheckParm ("-conwidth")) != 0)
		vid.conwidth = Q_atoi (com_argv[i + 1]);
	else
		vid.conwidth = width >= 640 ? width / 2 : width;
	vid.conwidth &= 0xfff8;
	if (vid.conwidth < 320)
		vid.conwidth = 320;
	vid.conheight = vid.conwidth * height / width;
	if ((i = COM_CheckParm ("-conheight")) != 0)
		vid.conheight = Q_atoi (com_argv[i + 1]);
	if (vid.conheight < 200)
		vid.conheight = 200;
	vid.width = vid.conwidth;
	vid.height = vid.conheight;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.numpages = 2;

	if ((s = getenv ("QUAKE_HOLD")) != NULL)
		hold_frame = atoi (s);
	hold_late = getenv ("QUAKE_HOLD_LATE") != NULL;
	if ((s = getenv ("QUAKE_STATS_MS")) != NULL)
		stats_ms = atoi (s);
	if ((s = getenv ("QUAKE_STATS_FRAMES")) != NULL)
		stats_frames = atoi (s);	/* a line every so many frames instead: the same frames each run */

	GL_Init ();
	gamma_palette (palette);
	VID_SetPalette (palette);
	Con_SafePrintf ("Video mode %dx%d initialized.\n", width, height);
	vid.recalc_refdef = 1;
}

/* ---- input ---- */

static int quake_key (int ch)
{
	static const char shifted[] = ")!@#$%^&*(";
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, K_LEFTARROW}, {MWKEY_RIGHT, K_RIGHTARROW}, {MWKEY_UP, K_UPARROW}, {MWKEY_DOWN, K_DOWNARROW},
		{MWKEY_ENTER, K_ENTER}, {'\n', K_ENTER}, {MWKEY_KP_ENTER, K_ENTER}, {MWKEY_ESCAPE, K_ESCAPE},
		{MWKEY_BACKSPACE, K_BACKSPACE}, {MWKEY_TAB, K_TAB}, {MWKEY_INSERT, K_INS}, {MWKEY_DELETE, K_DEL},
		{MWKEY_HOME, K_HOME}, {MWKEY_END, K_END}, {MWKEY_PAGEUP, K_PGUP}, {MWKEY_PAGEDOWN, K_PGDN},
		{MWKEY_LSHIFT, K_SHIFT}, {MWKEY_RSHIFT, K_SHIFT}, {MWKEY_LCTRL, K_CTRL}, {MWKEY_RCTRL, K_CTRL},
		{MWKEY_LALT, K_ALT}, {MWKEY_RALT, K_ALT}, {MWKEY_PAUSE, K_PAUSE},
	};
	const char *at;
	unsigned i;

	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A' + 'a';
	if (ch >= MWKEY_F1 && ch <= MWKEY_F12)
		return K_F1 + ch - MWKEY_F1;
	for (i = 0; i < sizeof other / sizeof other[0]; i++)
		if (other[i].ch == ch)
			return other[i].key;
	if (ch > 0 && ch < 128 && (at = strchr (shifted, ch)) != NULL)
		return '0' + (int)(at - shifted);
	return ch >= 32 && ch < 127 ? ch : 0;
}

/*
 * Asking the server costs two system calls and two task switches. The machine's own input
 * counters say when there can be anything: ask when they have moved, for a few calls after,
 * and a few times a second. The pointer's travel is read from the machine's input words
 * themselves (docs/input.md): the server's word of every move was a question and an answer
 * each, in every frame the view was turned in. The server says when the pointer comes over
 * the window and leaves it.
 */
void Sys_SendKeyEvents (void)
{
	static unsigned seen[4], asked_ms;
	static int ask = 8;
	const volatile unsigned *input;
	GR_EVENT e;
	int i, key;

	if (!opened)
		return;
	input = qglControl (REG_INPUT);
	if (mouse_inside) {
		int x = (int)input[0], y = (int)input[1];

		if (mouse_known)
			mouse_dx += x - mouse_x, mouse_dy += y - mouse_y;
		mouse_x = x, mouse_y = y, mouse_known = 1;
	}
	for (i = 2; i < 4; i++)		/* the buttons, and the count of keys */
		if (seen[i] != input[i]) {
			seen[i] = input[i];
			ask = 8;
		}
	if (!ask && VID_Milliseconds () - asked_ms >= 250)
		ask = 1;
	while (ask) {
		GrCheckNextEvent (&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = VID_Milliseconds ();
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1;
			mouse_known = 0;	/* from where it is now */
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE) {
				seglWindowChanged ();
				vid.recalc_refdef = 1;
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			if (e.button.changebuttons & GR_BUTTON_L)
				Key_Event (K_MOUSE1, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			if (e.button.changebuttons & GR_BUTTON_R)
				Key_Event (K_MOUSE2, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			if (e.button.changebuttons & GR_BUTTON_M)
				Key_Event (K_MOUSE3, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			key = quake_key (e.keystroke.ch);
			if (key)
				Key_Event (key, e.type == GR_EVENT_TYPE_KEY_DOWN);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Sys_Quit ();
		}
	}
}

void IN_Init (void) {}
void IN_Shutdown (void) {}
void IN_Commands (void) {}

/* With in_mouse 1 the pointer's travel over the window turns the view (it is not held there). */
void IN_Move (usercmd_t *cmd)
{
	float mx = mouse_dx * sensitivity.value, my = mouse_dy * sensitivity.value;

	mouse_dx = mouse_dy = 0;
	if (se_all_round) {
		/* Seated in a holodeck, where the room keeps the horizon level: the aim's rise is the
		   right hand's, when the host tracks hands. (Its turn stays the stick's: the room turns
		   with the game, and a hand's turn would chase itself.) */
		const volatile unsigned *host = seglHostState ();

		if (SEGL_HOST_HANDS (host)) {
			float rise = SEGL_HOST_HAND_PITCH (host, 1) * (360.0f / 65536.0f);

			V_StopPitchDrift ();
			cl.viewangles[PITCH] = rise > 70 ? -70 : rise < -80 ? 80 : -rise;
		}
	}
	if (!in_mouse.value || key_dest != key_game)
		return;
	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * mx;
	else
		cl.viewangles[YAW] -= m_yaw.value * mx;
	if (in_mlook.state & 1)
		V_StopPitchDrift ();
	if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * my;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	} else {
		cmd->forwardmove -= m_forward.value * my;
	}
}
